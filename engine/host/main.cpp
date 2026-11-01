#include "ps2/system.hpp"

#ifdef PS2_WINDOW
#include "window.hpp"
#endif
#ifdef PS2_PROFILE
#include "profile.hpp"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

void register_generated_code();
void register_generated_vu();

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: ps2run <elf> [--arg text]... [--frames n] [--ntsc] [--misses file]\n"
                 "              [--disc image.iso] [--memcard card.ps2] [--pal | --ntsc] [--trace-syscalls] [--trace-io] [--trace-threads]\n"
                 "              [--trace-iop] [--trace-iop-hle] [--frames-dir dir] [--frame-every n]\n"
                 "              [--press frame:button[+button][:frames]]... [--window] [--fullscreen]\n"
                 "              [--save-state frame:file] [--load-state file] [--widescreen] [--patches file]\n"
                 "              [--smap-tx-pcap capture.pcap] [--smap-rx-frame frame.bin --smap-rx-at frame]\n");
    return 2;
}

}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    std::vector<std::string> args{argv[1]};
    [[maybe_unused]] bool window = false, fullscreen = false;
    std::string load_path;
    std::string smap_pcap_path;
    std::string smap_rx_path;
    std::uint64_t smap_rx_at = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--arg" && i + 1 < argc) args.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) ps2::g_options.max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ntsc" || a == "--pal") {
            ps2::g_options.pal = a == "--pal";
            ps2::g_options.rate_forced = true;
        }
        else if (a == "--misses" && i + 1 < argc) ps2::g_options.misses_path = argv[++i];
        else if (a == "--trace-syscalls") ps2::g_options.trace_syscalls = true;
        else if (a == "--widescreen") ps2::g_options.widescreen = true;
        else if (a == "--patches" && i + 1 < argc) {
            if (!ps2::load_patches(argv[++i])) return usage();
        }
        else if (a == "--trace-io") ps2::g_options.trace_io = true;
        else if (a == "--trace-threads") ps2::g_options.trace_threads = true;
        else if (a == "--disc" && i + 1 < argc) ps2::g_options.disc_path = argv[++i];
        else if (a == "--memcard" && i + 1 < argc) ps2::g_options.memcard_path = argv[++i];
        else if (a == "--trace-iop") ps2::g_options.trace_iop_modules = true;
        else if (a == "--trace-iop-hle") ps2::g_options.trace_iop_hle = true;
        else if (a == "--smap-tx-pcap" && i + 1 < argc) smap_pcap_path = argv[++i];
        else if (a == "--smap-rx-frame" && i + 1 < argc) smap_rx_path = argv[++i];
        else if (a == "--smap-rx-at" && i + 1 < argc) smap_rx_at = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--frames-dir" && i + 1 < argc) ps2::g_options.frames_dir = argv[++i];
        else if (a == "--press" && i + 1 < argc) {
            ps2::Press press;
            if (!ps2::parse_press(argv[++i], press)) return usage();
            ps2::g_options.presses.push_back(press);
        }
        else if (a == "--axis" && i + 1 < argc) {
            ps2::AxisPress press;
            if (!ps2::parse_axis(argv[++i], press)) return usage();
            ps2::g_options.axis_presses.push_back(press);
        }
#ifdef PS2_WINDOW
        else if (a == "--window") window = true;
        else if (a == "--fullscreen") window = fullscreen = true;
#endif
        else if (a == "--frame-every" && i + 1 < argc) ps2::g_options.frame_every = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--save-state" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            if (colon == std::string::npos) return usage();
            ps2::g_options.save_state_frame = std::strtoull(spec.substr(0, colon).c_str(), nullptr, 10);
            ps2::g_options.save_state_path = spec.substr(colon + 1);
        }
        else if (a == "--load-state" && i + 1 < argc) load_path = argv[++i];
        else return usage();
    }

#ifdef PS2_WINDOW
    if (window && !open_window(fullscreen)) return 1;
#endif
    register_generated_code();
    register_generated_vu();
    if (!smap_rx_path.empty()) {
        std::FILE *input = std::fopen(smap_rx_path.c_str(), "rb");
        if (!input) { std::perror(smap_rx_path.c_str()); return 1; }
        std::vector<std::uint8_t> frame(1519);
        const std::size_t size = std::fread(frame.data(), 1, frame.size(), input);
        std::fclose(input);
        if (size < 14 || size > 1518) {
            std::fprintf(stderr, "SMAP RX diagnostic frame must be 14..1518 bytes\n");
            return 1;
        }
        frame.resize(size);
        ps2::g_options.on_vblank = [frame = std::move(frame), smap_rx_at, sent = false]() mutable {
            if (!sent && ps2::frame_count() >= smap_rx_at) {
                sent = ps2::inject_network_frame(frame.data(), frame.size());
                if (sent) std::fprintf(stderr, "[smap-rx] injected at frame %llu\n",
                                       static_cast<unsigned long long>(ps2::frame_count()));
            }
        };
    }
    std::shared_ptr<std::FILE> smap_pcap;
    if (!smap_pcap_path.empty()) {
        smap_pcap.reset(std::fopen(smap_pcap_path.c_str(), "wb"), std::fclose);
        if (!smap_pcap) {
            std::perror(smap_pcap_path.c_str());
            return 1;
        }
        const unsigned char header[24] = {
            0xD4, 0xC3, 0xB2, 0xA1, 2, 0, 4, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0xEE, 0x05, 0, 0, 1, 0, 0, 0
        };
        std::fwrite(header, 1, sizeof header, smap_pcap.get());
        ps2::g_options.on_smap_tx = [smap_pcap](const std::uint8_t *frame, std::size_t size, bool loopback) {
            static std::uint32_t number = 0;
            const std::uint32_t stamp = number++;
            const std::uint32_t length = static_cast<std::uint32_t>(size);
            const std::uint32_t record[4] = {stamp / 1000u, (stamp % 1000u) * 1000u, length, length};
            std::fwrite(record, sizeof(std::uint32_t), 4, smap_pcap.get());
            std::fwrite(frame, 1, size, smap_pcap.get());
            std::fprintf(stderr, "[smap-pcap] frame %u: %u bytes, loopback=%u\n", number, length, loopback);
        };
    }
    const auto emulate = [&]() -> int {
        std::uint32_t entry = 0;
        if (!ps2::load_elf(argv[1], entry)) {
            std::fprintf(stderr, "cannot load %s\n", argv[1]);
            return 1;
        }
        ps2::boot(entry, args);
        if (!load_path.empty() && !ps2::load_state(load_path)) return 1;
#ifdef PS2_PROFILE
        const char *profile = std::getenv("PS2_PROFILE");
        const char *profile_from = std::getenv("PS2_PROFILE_FROM");
        if (profile) start_profile(profile_from ? std::strtoull(profile_from, nullptr, 10) : 0);
        const int result = ps2::run();
        if (profile) write_profile(profile);
        return result;
#else
        return ps2::run();
#endif
    };
#ifdef PS2_WINDOW
    if (window) return run_window(emulate);
#endif
    return emulate();
}

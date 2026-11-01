#include "io.hpp"
#include "kernel.hpp"
#include "sio2.hpp"

#include "ps2/disc.hpp"
#include "ps2/system.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>

namespace ps2::iop::io {

namespace {

constexpr int kNoEntry = -2;
constexpr int kBadFd = -9;
constexpr int kTooMany = -24;

struct File {
    std::uint32_t sector = 0;
    std::uint32_t size = 0;
    std::uint64_t position = 0;
    std::vector<std::uint8_t> memory;
    bool in_memory = false;
};

struct Directory {
    struct Entry {
        std::string name;
        std::uint32_t mode = 0;
        std::uint32_t size = 0;
    };
    std::vector<Entry> entries;
    std::size_t position = 0;
};

std::map<int, File> g_files;
std::map<int, std::filesystem::path> g_capture_paths;
std::map<int, std::string> g_card_paths;
std::map<int, Directory> g_directories;

std::optional<std::filesystem::path> capture_path(const std::string &path) {
    if (!std::getenv("PS2_MC_CAPTURE_PROBE") || g_options.memcard_path.empty()) return std::nullopt;
    const std::filesystem::path base(g_options.memcard_path + ".capture");
    if (path == "mc0:" || path == "mc0:/") return base;
    constexpr std::size_t prefix_size = sizeof("mc0:/BWNETCNF") - 1;
    if (path.rfind("mc0:/BWNETCNF", 0) != 0 ||
        (path.size() > prefix_size && path[prefix_size] != '/')) return std::nullopt;
    const std::string relative = path.substr(5);
    if (relative.empty() || relative.find("..") != std::string::npos ||
        relative.find('\\') != std::string::npos) return std::nullopt;
    for (char ch : relative)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '/' && ch != '.' && ch != '_' && ch != '-')
            return std::nullopt;
    return base / relative;
}

std::string device_of(const std::string &path) {
    const auto colon = path.find(':');
    std::string d = colon == std::string::npos ? "" : path.substr(0, colon);
    for (char &ch : d) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    while (!d.empty() && std::isdigit(static_cast<unsigned char>(d.back()))) d.pop_back();
    return d;
}

int allocate() {
    for (int fd = 3; fd < 64; ++fd)
        if (!g_files.count(fd) && !g_directories.count(fd)) return fd;
    return kTooMany;
}

}

int open(const std::string &path, std::uint32_t flags) {
    const std::string device = device_of(path);
    File f;
    const auto captured = capture_path(path);
    if (device == "cdrom") {
        const auto entry = disc::find(path);
        if (!entry || entry->directory) return kNoEntry;
        f.sector = entry->sector;
        f.size = entry->size;
    } else if (device == "rom") {
        std::string name = path.substr(path.find(':') + 1);
        if (name == "ROMVER") {
            const std::string text = "0220EC20060210\n";
            f.memory.assign(text.begin(), text.end());
        } else if (name == "OSDSYS" || name.rfind("rom0:", 0) == 0) {
            return kNoEntry;
        } else {
            return kNoEntry;
        }
        f.in_memory = true;
        f.size = static_cast<std::uint32_t>(f.memory.size());
    } else if (captured) {
        const bool create = (flags & 0x200u) != 0;
        const bool truncate = (flags & 0x400u) != 0;
        if (!create && !std::filesystem::is_regular_file(*captured)) return kNoEntry;
        if (std::filesystem::is_regular_file(*captured) && !truncate) {
            std::ifstream in(*captured, std::ios::binary);
            f.memory.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        f.in_memory = true;
        f.size = static_cast<std::uint32_t>(f.memory.size());
    } else if (device == "mc" && std::getenv("PS2_MC_FS_PROBE")) {
        sio2::CardPath card;
        if ((flags & 3u) != 1u || !sio2::card_read_path(path, card) ||
            (card.mode & 0x10u) == 0) return kNoEntry;
        f.memory = std::move(card.data);
        f.in_memory = true;
        f.size = card.size;
        std::fprintf(stderr, "[mc-fs] open %s: %u bytes (read only)\n", path.c_str(), f.size);
    } else {
        std::fprintf(stderr, "[io] open %s (flags 0x%X) on unsupported device\n", path.c_str(), flags);
        return kNoEntry;
    }
    const int fd = allocate();
    if (fd < 0) return fd;
    g_files[fd] = std::move(f);
    if (captured) g_capture_paths[fd] = *captured;
    if (device == "mc" && !captured) g_card_paths[fd] = path;
    if (kernel::g_trace_hle) std::fprintf(stderr, "[io] open %s -> %d\n", path.c_str(), fd);
    return fd;
}

int close(int fd) {
    const auto file = g_files.find(fd);
    if (file == g_files.end()) return kBadFd;
    const auto path = g_capture_paths.find(fd);
    if (path != g_capture_paths.end()) {
        std::filesystem::create_directories(path->second.parent_path());
        std::ofstream out(path->second, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(file->second.memory.data()),
                  static_cast<std::streamsize>(file->second.memory.size()));
        if (!out) return -5;
        std::fprintf(stderr, "[mc-capture] %s: %zu bytes (sidecar only)\n",
                     path->second.string().c_str(), file->second.memory.size());
        g_capture_paths.erase(path);
    }
    if (g_card_paths.count(fd) && std::getenv("PS2_MC_FS_DEBUG"))
        std::fprintf(stderr, "[mc-fs] close %s fd %d\n", g_card_paths[fd].c_str(), fd);
    g_card_paths.erase(fd);
    g_files.erase(file);
    return 0;
}

int read(int fd, std::uint8_t *out, std::uint32_t size) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return kBadFd;
    File &f = it->second;
    if (f.position >= f.size) {
        if (g_card_paths.count(fd) && std::getenv("PS2_MC_FS_DEBUG"))
            std::fprintf(stderr, "[mc-fs] read %s fd %d requested %u returned 0 at eof\n",
                         g_card_paths[fd].c_str(), fd, size);
        return 0;
    }
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(size, f.size - f.position));
    if (f.in_memory) {
        std::memcpy(out, f.memory.data() + f.position, n);
    } else {
        const std::uint64_t start = f.position, end = f.position + n;
        const std::uint32_t first = static_cast<std::uint32_t>(start / disc::kSector);
        const std::uint32_t last = static_cast<std::uint32_t>((end + disc::kSector - 1) / disc::kSector);
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(last - first) * disc::kSector);
        disc::read(f.sector + first, last - first, buffer.data());
        std::memcpy(out, buffer.data() + (start - static_cast<std::uint64_t>(first) * disc::kSector), n);
    }
    f.position += n;
    if (g_card_paths.count(fd) && std::getenv("PS2_MC_FS_DEBUG"))
        std::fprintf(stderr, "[mc-fs] read %s fd %d requested %u returned %u position %llu\n",
                     g_card_paths[fd].c_str(), fd, size, n,
                     static_cast<unsigned long long>(f.position));
    return static_cast<int>(n);
}

int write(int fd, const std::uint8_t *data, std::uint32_t size) {
    if (fd == 1 || fd == 2) return static_cast<int>(size);
    if (g_capture_paths.count(fd)) {
        File &f = g_files.at(fd);
        if (size == 0) return 0;
        if (!data || f.position + size > 1u << 20) return -27;
        if (f.position + size > f.memory.size()) f.memory.resize(static_cast<std::size_t>(f.position + size));
        std::memcpy(f.memory.data() + f.position, data, size);
        f.position += size;
        f.size = static_cast<std::uint32_t>(f.memory.size());
        return static_cast<int>(size);
    }
    return g_files.count(fd) ? static_cast<int>(size) : kBadFd;
}

bool captured(int fd) { return g_capture_paths.count(fd) != 0; }

std::int64_t seek(int fd, std::int64_t offset, int whence) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return kBadFd;
    File &f = it->second;
    std::int64_t base = whence == 1 ? static_cast<std::int64_t>(f.position) : whence == 2 ? f.size : 0;
    const std::int64_t target = std::clamp<std::int64_t>(base + offset, 0, f.size);
    f.position = static_cast<std::uint64_t>(target);
    if (g_card_paths.count(fd) && std::getenv("PS2_MC_FS_DEBUG"))
        std::fprintf(stderr, "[mc-fs] seek %s fd %d offset %lld whence %d -> %lld\n",
                     g_card_paths[fd].c_str(), fd, static_cast<long long>(offset), whence,
                     static_cast<long long>(target));
    return target;
}

bool stat(const std::string &path, std::uint32_t &mode, std::uint32_t &size) {
    if (const auto captured = capture_path(path)) {
        if (!std::filesystem::exists(*captured)) return false;
        mode = std::filesystem::is_directory(*captured) ? 0x0027u : 0x0017u;
        size = std::filesystem::is_regular_file(*captured)
            ? static_cast<std::uint32_t>(std::filesystem::file_size(*captured)) : 0u;
        return true;
    }
    if (device_of(path) == "mc" && std::getenv("PS2_MC_FS_PROBE")) {
        sio2::CardPath card;
        if (!sio2::card_read_path(path, card)) return false;
        mode = (card.mode & 0x20u) ? 0x27u : 0x17u;
        size = card.size;
        return true;
    }
    if (device_of(path) != "cdrom") return false;
    const auto entry = disc::find(path);
    if (!entry) return false;
    mode = entry->directory ? 0x0027u : 0x0017u;
    size = entry->size;
    return true;
}

int make_dir(const std::string &path) {
    const auto captured = capture_path(path);
    if (!captured) return -1;
    std::filesystem::create_directories(*captured);
    return 0;
}

int remove(const std::string &path) {
    const auto captured = capture_path(path);
    if (!captured) return -1;
    std::error_code error;
    const bool removed = std::filesystem::remove(*captured, error);
    return error ? -5 : removed ? 0 : kNoEntry;
}

int rename_file(const std::string &from, const std::string &to) {
    const auto source = capture_path(from), target = capture_path(to);
    if (!source || !target) return -1;
    std::error_code error;
    std::filesystem::rename(*source, *target, error);
    if (error) return -5;
    std::fprintf(stderr, "[mc-capture] rename %s -> %s (sidecar only)\n",
                 source->string().c_str(), target->string().c_str());
    return 0;
}

int open_dir(const std::string &path) {
    const auto captured = capture_path(path);
    if ((!captured || !std::filesystem::is_directory(*captured)) &&
        !(device_of(path) == "mc" && std::getenv("PS2_MC_FS_PROBE"))) return kNoEntry;
    const int fd = allocate();
    if (fd < 0) return fd;
    Directory dir;
    if (captured && std::filesystem::is_directory(*captured)) {
        for (const auto &item : std::filesystem::directory_iterator(*captured)) {
            Directory::Entry entry;
            entry.name = item.path().filename().string();
            entry.mode = item.is_directory() ? 0x27u : 0x17u;
            entry.size = item.is_regular_file() ? static_cast<std::uint32_t>(item.file_size()) : 0u;
            dir.entries.push_back(std::move(entry));
        }
    } else {
        sio2::CardPath card;
        if (!sio2::card_read_path(path, card) || (card.mode & 0x20u) == 0) return kNoEntry;
        for (std::size_t at = 0; at + 512 <= card.data.size(); at += 512) {
            const std::uint8_t *p = card.data.data() + at;
            const std::uint32_t raw_mode = static_cast<std::uint32_t>(p[0] | (p[1] << 8));
            if ((raw_mode & 0x8000u) == 0) continue;
            const auto *name = reinterpret_cast<const char *>(p + 64);
            const std::string filename(name, strnlen(name, 448));
            if (filename == "." || filename == ".." || filename.empty()) continue;
            Directory::Entry entry;
            entry.name = filename;
            entry.mode = (raw_mode & 0x20u) ? 0x27u : 0x17u;
            entry.size = static_cast<std::uint32_t>(p[4]) |
                (static_cast<std::uint32_t>(p[5]) << 8) |
                (static_cast<std::uint32_t>(p[6]) << 16) |
                (static_cast<std::uint32_t>(p[7]) << 24);
            dir.entries.push_back(std::move(entry));
        }
    }
    g_directories[fd] = std::move(dir);
    std::fprintf(stderr, "[%s] dopen %s -> %d (%zu entries)\n",
                 captured ? "mc-capture" : "mc-fs", path.c_str(), fd, g_directories[fd].entries.size());
    return fd;
}

int close_dir(int fd) { return g_directories.erase(fd) ? 0 : kBadFd; }

int read_dir(int fd, std::string &name, std::uint32_t &mode, std::uint32_t &size) {
    auto it = g_directories.find(fd);
    if (it == g_directories.end()) return kBadFd;
    Directory &dir = it->second;
    if (dir.position >= dir.entries.size()) return 0;
    const auto &entry = dir.entries[dir.position++];
    name = entry.name;
    mode = entry.mode;
    size = entry.size;
    return 1;
}

void reset() { g_files.clear(); g_capture_paths.clear(); g_card_paths.clear(); g_directories.clear(); }

}

namespace ps2::iop::kernel {

namespace {

std::uint32_t arg(const Cpu &c, int i) { return i < 4 ? c.r[4 + i] : read32(c.r[29] + 16u + static_cast<std::uint32_t>(i - 4) * 4u); }

void def(const char *library, std::uint32_t index, const char *name, std::function<void(Cpu &)> body) {
    register_hle(library, index, [library, name, body = std::move(body)](Cpu &c) {
        trace_hle(library, name, c);
        body(c);
    });
}

}

void install_io() {
    io::reset();
    register_hle_library("ioman", 0x0104);
    def("ioman", 4, "open", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(io::open(ram_string(arg(c, 0)), arg(c, 1))); });
    def("ioman", 5, "close", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(io::close(static_cast<int>(arg(c, 0)))); });
    def("ioman", 6, "read", [](Cpu &c) {
        std::vector<std::uint8_t> buffer(arg(c, 2));
        const int n = io::read(static_cast<int>(arg(c, 0)), buffer.data(), arg(c, 2));
        for (int i = 0; i < n; ++i) write8(arg(c, 1) + static_cast<std::uint32_t>(i), buffer[static_cast<std::size_t>(i)]);
        c.r[2] = static_cast<std::uint32_t>(n);
    });
    def("ioman", 7, "write", [](Cpu &c) {
        if (arg(c, 0) == 1 || arg(c, 0) == 2) std::fprintf(stderr, "[iop-tty] %s", ram_string(arg(c, 1), arg(c, 2)).c_str());
        const std::uint32_t size = arg(c, 2);
        std::vector<std::uint8_t> buffer;
        if (io::captured(static_cast<int>(arg(c, 0))) && size <= 1u << 20) {
            buffer.resize(size);
            for (std::uint32_t i = 0; i < size; ++i) buffer[i] = read8(arg(c, 1) + i);
        }
        c.r[2] = static_cast<std::uint32_t>(io::write(static_cast<int>(arg(c, 0)),
                                                       buffer.empty() ? nullptr : buffer.data(), size));
    });
    def("ioman", 8, "lseek", [](Cpu &c) {
        c.r[2] = static_cast<std::uint32_t>(io::seek(static_cast<int>(arg(c, 0)), static_cast<std::int32_t>(arg(c, 1)), static_cast<int>(arg(c, 2))));
    });
    def("ioman", 16, "getstat", [](Cpu &c) {
        std::uint32_t mode = 0, size = 0;
        if (!io::stat(ram_string(arg(c, 0)), mode, size)) { c.r[2] = static_cast<std::uint32_t>(-2); return; }
        const std::uint32_t p = arg(c, 1);
        std::memset(ram_ptr(p), 0, 40);
        write32(p, mode);
        write32(p + 8, size);
        c.r[2] = 0;
    });
    def("ioman", 31, "devctl", [](Cpu &c) {
        const std::string path = ram_string(arg(c, 0));
        const std::uint32_t command = arg(c, 1);
        int result = -1;
        if (path == "mc0:") {
            if (command == 0x5001u) result = sio2::card_cluster_bytes();
            if (command == 0x5002u) result = sio2::card_free_clusters();
        }
        if (g_trace_hle)
            std::fprintf(stderr, "[io] devctl %s cmd 0x%X -> %d\n", path.c_str(), command, result);
        c.r[2] = static_cast<std::uint32_t>(result);
    });
    def("ioman", 20, "AddDrv", [](Cpu &c) {
        if (!std::getenv("PS2_IOMAN_DRV_PROBE")) { c.r[2] = static_cast<std::uint32_t>(-1); return; }
        const std::uint32_t device = arg(c, 0);
        std::fprintf(stderr, "[ioman-probe] AddDrv %s\n", ram_string(read32(device)).c_str());
        c.r[2] = 0;
    });
    def("ioman", 21, "DelDrv", [](Cpu &c) {
        if (!std::getenv("PS2_IOMAN_DRV_PROBE")) { c.r[2] = static_cast<std::uint32_t>(-1); return; }
        std::fprintf(stderr, "[ioman-probe] DelDrv %s\n", ram_string(arg(c, 0)).c_str());
        c.r[2] = 0;
    });
    def("ioman", 10, "remove", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(io::remove(ram_string(arg(c, 0)))); });
    def("ioman", 11, "mkdir", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(io::make_dir(ram_string(arg(c, 0)))); });
    def("ioman", 13, "dopen", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(io::open_dir(ram_string(arg(c, 0)))); });
    def("ioman", 14, "dclose", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(io::close_dir(static_cast<int>(arg(c, 0)))); });
    def("ioman", 15, "dread", [](Cpu &c) {
        std::string name;
        std::uint32_t mode = 0, size = 0;
        const int result = io::read_dir(static_cast<int>(arg(c, 0)), name, mode, size);
        if (result > 0) {
            const std::uint32_t p = arg(c, 1);
            std::memset(ram_ptr(p), 0, 320);
            write32(p, mode);
            write32(p + 8, size);
            for (std::size_t i = 0; i < std::min<std::size_t>(name.size(), 255); ++i)
                write8(p + 64 + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(name[i]));
        }
        c.r[2] = static_cast<std::uint32_t>(result);
    });
    def("ioman", 17, "chstat", [](Cpu &c) {
        if (!std::getenv("PS2_MC_CAPTURE_PROBE")) { c.r[2] = static_cast<std::uint32_t>(-1); return; }
        std::uint32_t mode = 0, size = 0;
        c.r[2] = io::stat(ram_string(arg(c, 0)), mode, size) ? 0 : static_cast<std::uint32_t>(-2);
    });
    def("ioman", 25, "rename", [](Cpu &c) {
        c.r[2] = static_cast<std::uint32_t>(io::rename_file(ram_string(arg(c, 0)), ram_string(arg(c, 1))));
    });
    for (std::uint32_t i : {9u, 12u, 27u})
        def("ioman", i, "ioman", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(-1); });
}

}

namespace ps2::iop::io {

void state(ps2::state::Stream &s) {
    s.tag("iopio");
    s.map(g_files, [&s](File &f) {
        s.pod(f.sector);
        s.pod(f.size);
        s.pod(f.position);
        s.vec(f.memory);
        s.pod(f.in_memory);
    });
}

}

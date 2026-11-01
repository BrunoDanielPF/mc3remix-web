#include "hw.hpp"

#include "iop.hpp"
#include "kernel.hpp"
#include "sio2.hpp"
#include "spu2.hpp"

#include "ps2/state.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace ps2::iop {

namespace {

alignas(16) std::uint8_t g_scratchpad[1024];

bool is_ram(std::uint32_t p) { return p < 0x00800000u; }
bool is_scratchpad(std::uint32_t p) { return (p & ~0x3FFu) == 0x1F800000u; }

}

std::uint8_t read8(std::uint32_t a) {
    const std::uint32_t p = phys(a);
    if (is_ram(p)) return g_ram[p & (kRamSize - 1)];
    if (is_scratchpad(p)) return g_scratchpad[p & 0x3FFu];
    return static_cast<std::uint8_t>(hw::read(p, 1));
}

std::uint16_t read16(std::uint32_t a) {
    const std::uint32_t p = phys(a);
    std::uint16_t v;
    if (is_ram(p)) { std::memcpy(&v, g_ram + (p & (kRamSize - 1)), 2); return v; }
    if (is_scratchpad(p)) { std::memcpy(&v, g_scratchpad + (p & 0x3FEu), 2); return v; }
    return static_cast<std::uint16_t>(hw::read(p, 2));
}

std::uint32_t read32(std::uint32_t a) {
    const std::uint32_t p = phys(a);
    std::uint32_t v;
    if (is_ram(p)) { std::memcpy(&v, g_ram + (p & (kRamSize - 1)), 4); return v; }
    if (is_scratchpad(p)) { std::memcpy(&v, g_scratchpad + (p & 0x3FCu), 4); return v; }
    return hw::read(p, 4);
}

void write8(std::uint32_t a, std::uint8_t v) {
    const std::uint32_t p = phys(a);
    if (is_ram(p)) { g_ram[p & (kRamSize - 1)] = v; return; }
    if (is_scratchpad(p)) { g_scratchpad[p & 0x3FFu] = v; return; }
    hw::write(p, v, 1);
}

void write16(std::uint32_t a, std::uint16_t v) {
    const std::uint32_t p = phys(a);
    if (is_ram(p)) { std::memcpy(g_ram + (p & (kRamSize - 1)), &v, 2); return; }
    if (is_scratchpad(p)) { std::memcpy(g_scratchpad + (p & 0x3FEu), &v, 2); return; }
    hw::write(p, v, 2);
}

void write32(std::uint32_t a, std::uint32_t v) {
    const std::uint32_t p = phys(a);
    if (is_ram(p)) { std::memcpy(g_ram + (p & (kRamSize - 1)), &v, 4); return; }
    if (is_scratchpad(p)) { std::memcpy(g_scratchpad + (p & 0x3FCu), &v, 4); return; }
    hw::write(p, v, 4);
}

namespace hw {

namespace {

constexpr std::uint32_t kIStat = 0x1F801070u, kIMask = 0x1F801074u, kICtrl = 0x1F801078u;
constexpr std::uint32_t kDicr = 0x1F8010F4u, kDicr2 = 0x1F801574u;

std::uint32_t g_istat = 0, g_imask = 0, g_ictrl = 1;
std::uint32_t g_dicr = 0, g_dicr2 = 0;
std::map<std::uint32_t, std::uint32_t> g_regs;
std::set<std::uint32_t> g_reported;

struct Dma {
    std::uint32_t madr, bcr, chcr, tadr;
};
Dma g_dma[14];
std::uint32_t g_ohci[0x18];

int dma_channel(std::uint32_t a, std::uint32_t &reg) {
    if (a >= 0x1F801080u && a < 0x1F8010F0u) {
        reg = (a & 0xFu) >> 2;
        return static_cast<int>((a - 0x1F801080u) >> 4);
    }
    if (a >= 0x1F801500u && a < 0x1F801570u) {
        reg = (a & 0xFu) >> 2;
        return 7 + static_cast<int>((a - 0x1F801500u) >> 4);
    }
    return -1;
}

void report(const char *what, std::uint32_t a, std::uint32_t v) {
    if (g_reported.insert(a).second)
        std::fprintf(stderr, "[iop-io] %s 0x%08X = 0x%X (pc 0x%08X)\n", what, a, v, g_cpu.pc);
}

void trace_smap(const char *what, std::uint32_t a, std::uint32_t v, unsigned size) {
    if (a == 0x10000028u || a == 0x10001004u || a == 0x1000100Cu ||
        a == 0x10001010u || a == 0x10002008u || a == 0x1000200Au ||
        a == 0x1000205Cu || a == 0x1000205Eu ||
        (a >= 0x10003000u && a < 0x10003200u)) {
        if (!std::getenv("PS2_SMAP_TRACE")) return;
        std::fprintf(stderr, "[smap-probe] %s%u 0x%08X = 0x%X pc 0x%08X\n", what, size * 8, a, v, g_cpu.pc);
    }
}

std::uint32_t probe_phy_bmsr() {
    if (!std::getenv("PS2_DEV9_LINK_PROBE")) return g_regs[0xF0000001u];
    // Diagnostic comparison: a PHY still in the DEV9 self-test loopback mode
    // must not claim an external cable link.
    if (std::getenv("PS2_DEV9_LINK_STRICT_PROBE") && (g_regs[0xF0000000u] & 0x4000u))
        return 0x7869u;
    return 0x786Du;
}

void start_dma(unsigned ch) {
    Dma &d = g_dma[ch];
    if (kernel::g_trace_modules) std::fprintf(stderr, "[iop-dma] start ch %u madr 0x%08X bcr 0x%08X chcr 0x%08X\n", ch, d.madr, d.bcr, d.chcr);
    const std::uint32_t words = (d.bcr & 0xFFFFu) * ((d.bcr >> 16) ? (d.bcr >> 16) : 1u);
    if (ch == 8 && std::getenv("PS2_SMAP_RX_PROBE") &&
        std::getenv("PS2_DEV9_PROBE") && std::getenv("PS2_DEV9_SMAP_PROBE") &&
        !(d.chcr & 1u) && (g_regs[0x10001030u] & 2u) && words <= 4096u) {
        const std::uint32_t dest = phys(d.madr);
        if (dest <= kRamSize && words * 4u <= kRamSize - dest) {
            std::uint32_t ptr = g_regs[0xF0000208u] & 0x3FFFu;
            for (std::uint32_t i = 0; i < words; ++i) {
                const auto it = g_regs.find(0xE0010000u + ptr);
                const std::uint32_t value = it == g_regs.end() ? 0 : it->second;
                std::memcpy(g_ram + dest + i * 4u, &value, 4);
                ptr = (ptr + 4u) & 0x3FFFu;
            }
            g_regs[0xF0000208u] = ptr;
            g_regs[0xF000020Cu] = dest;
            g_regs[0x10001030u] &= ~2u;
            if (std::getenv("PS2_SMAP_TRACE"))
                std::fprintf(stderr, "[smap-probe] DMA8 RX %u bytes to 0x%08X\n", words * 4u, dest);
            dma_done(ch);
            return;
        }
    }
    if (ch == 8 && std::getenv("PS2_SMAP_DMA_PROBE") &&
        std::getenv("PS2_DEV9_PROBE") && std::getenv("PS2_DEV9_SMAP_PROBE") &&
        (d.chcr & 1u) && (g_regs[0x10001000u] & 2u) && words <= 4096u) {
        const std::uint32_t source = phys(d.madr);
        if (source <= kRamSize && words * 4u <= kRamSize - source) {
            std::uint32_t ptr = g_regs[0x10001004u] & 0x0FFFu;
            for (std::uint32_t i = 0; i < words; ++i) {
                g_regs[0xE0000000u + ptr] = ram32(source + i * 4u);
                ptr = (ptr + 4u) & 0x0FFFu;
            }
            g_regs[0x10001004u] = ptr;
            g_regs[0x10001000u] &= ~2u; // TXFIFO DMAEN clears on completion.
            if (std::getenv("PS2_SMAP_TRACE")) {
                std::fprintf(stderr, "[smap-probe] DMA8 TX %u bytes from 0x%08X, FIFO ptr 0x%X\n", words * 4u, source, ptr);
                std::fprintf(stderr, "[smap-probe] DMA8 first bytes:");
                for (std::uint32_t i = 0; i < std::min<std::uint32_t>(words * 4u, 48u); ++i)
                    std::fprintf(stderr, " %02X", g_ram[source + i]);
                std::fprintf(stderr, "\n");
            }
            dma_done(ch);
            return;
        }
    }
    if (ch == 4 || ch == 7) {
        spu2::dma(ch == 4 ? 0 : 1, d.madr, words, (d.chcr & 1u) != 0, static_cast<int>(ch));
        return;
    }
    if (ch == 11 || ch == 12) {
        sio2::arm_dma(static_cast<int>(ch), d.madr, d.bcr & 0xFFFFu, d.bcr >> 16);
        return;
    }
    std::fprintf(stderr, "[iop-dma] channel %u start madr 0x%08X bcr 0x%08X chcr 0x%08X (not emulated)\n", ch, d.madr, d.bcr, d.chcr);
    dma_done(ch);
}

}

void reset() {
    kernel::register_job(kernel::kJobDmaDone, [](const kernel::Job &j) { dma_done(j.a); });
    g_istat = g_imask = g_dicr = g_dicr2 = 0;
    g_ictrl = 1;
    g_regs.clear();
    std::memset(g_dma, 0, sizeof g_dma);
    std::memset(g_scratchpad, 0, sizeof g_scratchpad);
    spu2::reset();
    sio2::reset();
    std::memset(g_ohci, 0, sizeof g_ohci);
}

bool inject_smap_frame(const std::uint8_t *frame, std::size_t size) {
    if (!std::getenv("PS2_SMAP_RX_PROBE") || !std::getenv("PS2_DEV9_PROBE") ||
        !std::getenv("PS2_DEV9_SMAP_PROBE") || !frame || size < 14 || size > 1518 ||
        g_regs[0x1000103Cu] != 0u) return false; // one in flight until FIFO drain is modeled
    const std::uint32_t index = g_regs[0xF0000204u] & 63u;
    const std::uint32_t bd = 0x10003200u + index * 8u;
    const std::uint32_t status = g_regs[bd] & 0xFFFFu;
    const bool swapped = (status & 0x0080u) != 0;
    if (!(status & (swapped ? 0x0080u : 0x8000u))) return false;
    const std::uint32_t ptr = g_regs[0xF0000200u] & 0x3FFFu;
    const std::uint32_t rounded = (static_cast<std::uint32_t>(size) + 3u) & ~3u;
    for (std::uint32_t i = 0; i < rounded; i += 4u) {
        std::uint32_t word = 0;
        for (std::uint32_t j = 0; j < 4u; ++j)
            if (i + j < size) word |= static_cast<std::uint32_t>(frame[i + j]) << (j * 8u);
        g_regs[0xE0010000u + ((ptr + i) & 0x3FFFu)] = word;
    }
    const auto encode = [swapped](std::uint32_t value) {
        return swapped ? ((value & 0xFFu) << 8u) | ((value >> 8u) & 0xFFu) : value;
    };
    g_regs[bd + 4u] = encode(static_cast<std::uint32_t>(size));
    g_regs[bd + 6u] = encode(0x4000u + ptr);
    g_regs[bd] = 0;
    g_regs[0xF0000200u] = (ptr + rounded) & 0x3FFFu;
    g_regs[0xF0000210u] = ptr;
    g_regs[0xF0000214u] = static_cast<std::uint32_t>(size);
    g_regs[0xF0000204u] = (index + 1u) & 63u;
    ++g_regs[0x1000103Cu];
    g_regs[0x10000028u] |= 0x20u;
    if (g_regs[0x1000002Au] & 0x20u) raise_interrupt(13u);
    if (std::getenv("PS2_SMAP_TRACE"))
        std::fprintf(stderr, "[smap-probe] RX injected %zu bytes BD %u ptr 0x%X status 0x%X\n", size, index, 0x4000u + ptr, status);
    return true;
}

void raise_interrupt(unsigned line) {
    if (line < 32) g_istat |= 1u << line;
}

void set_interrupt_mask(unsigned line, bool enabled) {
    if (line < 32) {
        if (enabled) g_imask |= 1u << line;
        else g_imask &= ~(1u << line);
        return;
    }
    if (enabled) {
        g_imask |= 8u;
        g_dicr |= 0x800000u;
    }
    if (line < 39) {
        const std::uint32_t bit = 1u << (16 + line - 32);
        if (enabled) g_dicr |= bit;
        else g_dicr &= ~bit;
    } else if (line >= 40 && line < 46) {
        const std::uint32_t bit = 1u << (16 + line - 40);
        if (enabled) g_dicr2 |= bit;
        else g_dicr2 &= ~bit;
    }
}

int next_pending_interrupt() {
    if (!g_ictrl) return -1;
    const std::uint32_t pending = g_istat & g_imask;
    if (pending & 8u) {
        const std::uint32_t flags = (g_dicr >> 24) & (g_dicr >> 16) & 0x7Fu;
        if (flags) return 32 + __builtin_ctz(flags);
        const std::uint32_t flags2 = (g_dicr2 >> 24) & (g_dicr2 >> 16) & 0x3Fu;
        if (flags2) return 40 + __builtin_ctz(flags2);
        g_istat &= ~8u;
    }
    const std::uint32_t rest = g_istat & g_imask & ~8u;
    return rest ? __builtin_ctz(rest) : -1;
}

void acknowledge_interrupt(unsigned line) {
    if (line < 32) {
        g_istat &= ~(1u << line);
        return;
    }
    if (line < 39) g_dicr &= ~(1u << (24 + line - 32));
    else if (line >= 40) g_dicr2 &= ~(1u << (24 + line - 40));
    if (!(((g_dicr >> 24) & 0x7Fu) | ((g_dicr2 >> 24) & 0x3Fu))) g_istat &= ~8u;
}

void dma_done(unsigned ch) {
    g_dma[ch].chcr &= ~0x01000000u;
    if (ch < 7) g_dicr |= 1u << (24 + ch);
    else g_dicr2 |= 1u << (24 + ch - 7);
    g_istat |= 8u;
}

std::uint32_t ohci_read(std::uint32_t a) {
    const std::uint32_t reg = (a - 0x1F801600u) >> 2;
    switch (reg) {
    case 0x00: return 0x110u;
    case 0x02: return g_ohci[reg] & ~1u;
    case 0x0E: return static_cast<std::uint32_t>(kernel::now() / 36864u) & 0xFFFFu;
    case 0x12: return 0x00000202u;
    default: return reg < 0x18 ? g_ohci[reg] : 0u;
    }
}

void ohci_write(std::uint32_t a, std::uint32_t v) {
    const std::uint32_t reg = (a - 0x1F801600u) >> 2;
    if (reg >= 0x18) return;
    if (reg == 0x03) g_ohci[reg] &= ~v;
    else if (reg == 0x05) g_ohci[0x04] &= ~v;
    else if (reg == 0x04) g_ohci[reg] |= v;
    else g_ohci[reg] = v;
}

std::uint32_t read(std::uint32_t a, unsigned size) {
    if (a == 0x10001200u && size == 4 && std::getenv("PS2_SMAP_RX_PROBE")) {
        const std::uint32_t ptr = g_regs[0xF0000208u] & 0x3FFFu;
        const auto it = g_regs.find(0xE0010000u + ptr);
        g_regs[0xF0000208u] = (ptr + 4u) & 0x3FFFu;
        return it == g_regs.end() ? 0 : it->second;
    }
    // Diagnostic only: let DEV9.IRX identify an Expansion Bay adapter and
    // the SPEED revision. The separate SMAP flag tests the next capability
    // gate; neither flag implements Ethernet or should be used for gameplay.
    if (size == 2 && (a == 0x1F80146Eu || a == 0x10000002u || a == 0x10000004u) &&
        std::getenv("PS2_DEV9_PROBE")) {
        if (a == 0x1F80146Eu) return 0x30u;
        if (a == 0x10000002u) return 0x11u;
        if (a == 0x10000004u && std::getenv("PS2_DEV9_SMAP_PROBE")) return 0x01u;
    }
    if (a == 0x1000002Eu && size == 1 && std::getenv("PS2_DEV9_PROBE") &&
        std::getenv("PS2_DEV9_SMAP_PROBE") && std::getenv("PS2_DEV9_EEPROM_PROBE")) {
        const std::uint32_t clock = g_regs[0xF0000100u];
        if (clock >= 11u && clock < 75u) {
            // Diagnostic locally administered MAC 02:00:00:00:00:slot.
            // Separate room slots require distinct hardware addresses.
            unsigned slot = 1;
            if (const char *value = std::getenv("PS2_SMAP_MAC_SLOT")) {
                const int parsed = std::atoi(value);
                if (parsed >= 1 && parsed <= 8) slot = static_cast<unsigned>(parsed);
            }
            const std::uint16_t high = static_cast<std::uint16_t>(slot << 8u);
            const std::uint16_t words[4] = {0x0002u, 0x0000u, high,
                                            static_cast<std::uint16_t>(high + 2u)};
            const std::uint32_t bit = clock - 11u;
            if (bit == 63u && std::getenv("PS2_SMAP_TRACE"))
                std::fprintf(stderr, "[smap-probe] EEPROM MAC read complete\n");
            return ((words[bit / 16u] >> (15u - bit % 16u)) & 1u) << 4;
        }
        return 0;
    }
    if (a >= 0x1F900000u && a < 0x1F900800u) return spu2::read(a, size);
    if (a >= 0x1F801600u && a < 0x1F801660u) return ohci_read(a);
    if (a >= 0x1F808200u && a < 0x1F808280u) return sio2::read(a);
    if (a == kIStat) return g_istat;
    if (a == kIMask) return g_imask;
    if (a == kICtrl) {
        const std::uint32_t v = g_ictrl;
        g_ictrl = 0;
        return v;
    }
    if (a == kDicr) return g_dicr;
    if (a == kDicr2) return g_dicr2;
    std::uint32_t reg = 0;
    const int ch = dma_channel(a, reg);
    if (ch >= 0) {
        const Dma &d = g_dma[ch];
        return (reg == 0 ? d.madr : reg == 1 ? d.bcr : reg == 2 ? d.chcr : d.tadr) >> ((a & 3u) * 8);
    }
    if ((a >= 0x1F801100u && a < 0x1F801130u) || (a >= 0x1F801480u && a < 0x1F8014B0u)) {
        if ((a & 0xFu) == 0) return static_cast<std::uint32_t>(kernel::now());
    }
    const auto it = g_regs.find(a);
    const std::uint32_t v = it == g_regs.end() ? 0 : it->second;
    report("read", a, v);
    trace_smap("read", a, v, size);
    return v;
}

void write(std::uint32_t a, std::uint32_t v, unsigned size) {
    if (a >= 0x1F900000u && a < 0x1F900800u) { spu2::write(a, v, size); return; }
    if (a >= 0x1F801600u && a < 0x1F801660u) { ohci_write(a, v); return; }
    if (a >= 0x1F808200u && a < 0x1F808280u) { sio2::write(a, v); return; }
    if (a == kIStat) { g_istat &= v; return; }
    if (a == kIMask) { g_imask = v; return; }
    if (a == kICtrl) { g_ictrl = v & 1u; return; }
    if (a == kDicr) {
        g_dicr = (g_dicr & 0x7F000000u & ~(v & 0x7F000000u)) | (v & 0x00FFFFFFu);
        return;
    }
    if (a == kDicr2) {
        g_dicr2 = (g_dicr2 & 0x7F000000u & ~(v & 0x7F000000u)) | (v & 0x00FFFFFFu);
        return;
    }
    std::uint32_t reg = 0;
    const int ch = dma_channel(a, reg);
    if (ch >= 0) {
        Dma &d = g_dma[ch];
        std::uint32_t &target = reg == 0 ? d.madr : reg == 1 ? d.bcr : reg == 2 ? d.chcr : d.tadr;
        const unsigned shift = (a & 3u) * 8;
        const std::uint32_t mask = size >= 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u) << shift;
        target = (target & ~mask) | ((v << shift) & mask);
        if (reg == 2 && (d.chcr & 0x01000000u) && (mask & 0x01000000u)) start_dma(static_cast<unsigned>(ch));
        return;
    }
    report("write", a, v);
    trace_smap("write", a, v, size);
    const bool smap_probe = a >= 0x10000000u && a < 0x10004000u &&
        std::getenv("PS2_DEV9_PROBE") && std::getenv("PS2_DEV9_SMAP_PROBE");
    if (smap_probe && std::getenv("PS2_DEV9_EEPROM_PROBE") && size == 1) {
        if (a == 0x1000002Cu && (v & 0xE0u) == 0xE0u) {
            g_regs[0xF0000100u] = 0;
        } else if (a == 0x1000002Eu && (v & 0xC0u) == 0xC0u) {
            ++g_regs[0xF0000100u];
        }
    }
    if (smap_probe && a == 0x10000128u && size == 2) {
        g_regs[0x10000028u] &= ~v; // SPEED interrupt status is write-one-to-clear.
        return;
    }
    if (smap_probe && a == 0x10001100u && size == 4) {
        const std::uint32_t ptr = g_regs[0x10001004u] & 0x0FFFu;
        g_regs[0xE0000000u + ptr] = v; // Serialized diagnostic TX FIFO.
        g_regs[0x10001004u] = (ptr + 4u) & 0x0FFFu;
        return;
    }
    if (smap_probe && a == 0x10001010u && size == 1) {
        ++g_regs[0x1000100Cu];
        return;
    }
    if (smap_probe && std::getenv("PS2_SMAP_RX_PROBE") && a == 0x10001034u && size == 2) {
        g_regs[a] = v;
        g_regs[0xF0000208u] = (v - 0x4000u) & 0x3FFFu;
        return;
    }
    if (smap_probe && std::getenv("PS2_SMAP_RX_PROBE") && a == 0x10001040u && size == 1) {
        if (std::getenv("PS2_SMAP_TRACE")) {
            const std::uint32_t dest = g_regs[0xF000020Cu];
            const std::uint32_t ptr = g_regs[0xF0000210u];
            const std::uint32_t length = g_regs[0xF0000214u];
            std::uint32_t mismatch = length;
            if (length <= 1518u && dest <= kRamSize && length <= kRamSize - dest) {
                for (std::uint32_t i = 0; i < length; ++i) {
                    const std::uint32_t off = (ptr + i) & 0x3FFFu;
                    const auto it = g_regs.find(0xE0010000u + (off & ~3u));
                    const std::uint32_t word = it == g_regs.end() ? 0 : it->second;
                    if (g_ram[dest + i] != static_cast<std::uint8_t>(word >> (8u * (off & 3u)))) {
                        mismatch = i;
                        break;
                    }
                }
            }
            std::fprintf(stderr, "[smap-probe] RX consumed %u bytes, first mismatch %u\n", length, mismatch);
        }
        if (g_regs[0x1000103Cu]) --g_regs[0x1000103Cu];
        return;
    }
    if (smap_probe && a == 0x10002008u && (size == 2 || size == 4) && (v & 0x8000u)) {
        const auto value_at = [](std::uint32_t address) {
            const auto it = g_regs.find(address);
            return it == g_regs.end() ? 0u : it->second;
        };
        std::uint32_t bd = 0, index = 0, status = 0;
        for (std::uint32_t step = 0; step < 64u; ++step) {
            const std::uint32_t candidate = (g_regs[0xF0000300u] + step) & 63u;
            const std::uint32_t address = 0x10003000u + candidate * 8u;
            const std::uint32_t candidate_status = value_at(address) & 0xFFFFu;
            if (candidate_status & 0x8080u) {
                bd = address;
                index = candidate;
                status = candidate_status;
                break;
            }
        }
        const bool swapped_bd = (status & 0x0080u) != 0;
        const std::uint32_t raw_length = value_at(bd + 4u) & 0xFFFFu;
        const std::uint32_t length = swapped_bd ? ((raw_length & 0xFFu) << 8) | (raw_length >> 8) : raw_length;
        const std::uint32_t raw_pointer = value_at(bd + 6u) & 0xFFFFu;
        const std::uint32_t pointer = swapped_bd ? ((raw_pointer & 0xFFu) << 8) | (raw_pointer >> 8) : raw_pointer;
        const bool ready = bd && (status & (swapped_bd ? 0x0080u : 0x8000u));
        const bool available = g_regs[0x1000100Cu] && pointer >= 0x1000u &&
            pointer < 0x2000u && length >= 14u && length <= 1518u;
        if (ready && available) {
            const bool loopback = (g_regs[0xF0000000u] & 0x4000u) != 0;
            std::uint8_t frame[1518];
            const std::uint32_t first = (pointer - 0x1000u) & 0x0FFFu;
            for (std::uint32_t i = 0; i < length; ++i) {
                const std::uint32_t offset = (first + i) & 0x0FFFu;
                const auto word = g_regs.find(0xE0000000u + (offset & ~3u));
                const std::uint32_t value = word == g_regs.end() ? 0 : word->second;
                frame[i] = static_cast<std::uint8_t>(value >> ((offset & 3u) * 8u));
            }
            g_regs[bd] &= ~(swapped_bd ? 0x0080u : 0x8000u);
            g_regs[0xF0000300u] = (index + 1u) & 63u;
            --g_regs[0x1000100Cu];
            // The startup frame is transmitted with PHY loopback enabled.
            // RXEND, TXEND and TXDNV are observed by its self-test.
            g_regs[0x10000028u] |= loopback ? 0x34u : 0x14u;
            if (g_options.on_smap_tx) g_options.on_smap_tx(frame, length, loopback);
            if (std::getenv("PS2_SMAP_TRACE")) {
                std::fprintf(stderr, "[smap-probe] tx BD %u length %u loopback %u PHY BMCR 0x%04X status 0x%X\n",
                             index, length, loopback, g_regs[0xF0000000u] & 0xFFFFu, g_regs[0x10000028u]);
                std::fprintf(stderr, "[smap-probe] TX frame first bytes:");
                for (std::uint32_t i = 0; i < std::min<std::uint32_t>(length, 48u); ++i)
                    std::fprintf(stderr, " %02X", frame[i]);
                std::fprintf(stderr, "\n");
            }
        }
        g_regs[a] = v & ~0x8000u;
        return;
    }
    // The SMAP FIFO reset bit clears when the reset completes. Keep this
    // narrow and opt-in while mapping the driver's remaining dependencies.
    if (size == 1 && (a == 0x10001000u || a == 0x10001030u) &&
        std::getenv("PS2_DEV9_PROBE") && std::getenv("PS2_DEV9_SMAP_PROBE")) {
        g_regs[a] = v & ~1u;
        if (a == 0x10001000u && (v & 1u)) {
            g_regs[0x10001004u] = 0;
            g_regs[0x1000100Cu] = 0;
        }
        return;
    }
    if ((size == 2 || size == 4) && a == 0x10002000u && std::getenv("PS2_DEV9_PROBE") &&
        std::getenv("PS2_DEV9_SMAP_PROBE")) {
        g_regs[a] = v & ~0x2000u; // EMAC3 reset completes for halfword and word writes.
        return;
    }
    if (size == 4 && a == 0x1000205Cu && std::getenv("PS2_DEV9_PROBE") &&
        std::getenv("PS2_DEV9_SMAP_PROBE")) {
        // The SMAP driver issues the same PHY operation as one 32-bit word:
        // command in the upper halfword, data in the lower halfword.
        const std::uint32_t command = v >> 16;
        const std::uint32_t phy_reg = command & 0x1Fu;
        const std::uint32_t key = 0xF0000000u + phy_reg;
        if (command & 0x2000u) {
            g_regs[key] = (v & 0xFFFFu) & (phy_reg == 0 ? ~0x8000u : 0xFFFFu);
            if (std::getenv("PS2_SMAP_TRACE"))
                std::fprintf(stderr, "[smap-probe] PHY write32 reg %u = 0x%04X\n", phy_reg, g_regs[key]);
        }
        // BMSR link and auto-negotiation bits are a separate diagnostic gate.
        const std::uint32_t data = (command & 0x1000u)
            ? (phy_reg == 1u ? probe_phy_bmsr() : g_regs[key]) : (v & 0xFFFFu);
        g_regs[a] = ((command | 0x8000u) << 16) | data;
        g_regs[0x1000205Eu] = command | 0x8000u;
        return;
    }
    if (size == 2 && a == 0x1000205Eu && std::getenv("PS2_DEV9_PROBE") &&
        std::getenv("PS2_DEV9_SMAP_PROBE")) {
        // EMAC3 stores the PHY command in the upper address halfword; its
        // data halfword is at the lower address (big-endian register view).
        const std::uint32_t command = v;
        const std::uint32_t phy_reg = command & 0x1Fu;
        // Keep probe PHY values in the serialized register map, outside the
        // IOP physical address range, so save states remain compatible.
        const std::uint32_t key = 0xF0000000u + phy_reg;
        if (command & 0x2000u) {
            g_regs[key] = g_regs[0x1000205Cu] & (phy_reg == 0 ? ~0x8000u : 0xFFFFu);
            if (std::getenv("PS2_SMAP_TRACE"))
                std::fprintf(stderr, "[smap-probe] PHY write16 reg %u = 0x%04X\n", phy_reg, g_regs[key]);
        }
        if (command & 0x1000u)
            g_regs[0x1000205Cu] = phy_reg == 1u ? probe_phy_bmsr() : g_regs[key];
        g_regs[a] = command | 0x8000u; // PHY operation complete.
        return;
    }
    g_regs[a] = v;
}

std::uint16_t *spu2_ram() { return spu2::ram(); }

}

}

namespace ps2::iop::hw {

void state(ps2::state::Stream &s) {
    s.tag("iophw");
    s.pod(g_istat);
    s.pod(g_imask);
    s.pod(g_ictrl);
    s.pod(g_dicr);
    s.pod(g_dicr2);
    s.map(g_regs);
    s.pod(g_dma);
    s.pod(g_ohci);
    s.pod(g_scratchpad);
}

}

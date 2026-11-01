#pragma once

#include <cstdint>
#include <cstddef>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::hw {

void state(ps2::state::Stream &s);

void reset();
void raise_interrupt(unsigned line);
void set_interrupt_mask(unsigned line, bool enabled);
int next_pending_interrupt();
void acknowledge_interrupt(unsigned line);
void dma_done(unsigned channel);
// Inject one Ethernet frame into the opt-in diagnostic SMAP RX ring.
// Call on the emulation thread; false means the ring is unavailable or full.
bool inject_smap_frame(const std::uint8_t *frame, std::size_t size);

std::uint32_t read(std::uint32_t a, unsigned size);
void write(std::uint32_t a, std::uint32_t v, unsigned size);

std::uint16_t *spu2_ram();

}

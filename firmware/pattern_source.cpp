// SPDX-License-Identifier: Apache-2.0
// Used on a second external Pico or on a separate internal loopback bus.
#include "pattern_source.hpp"
#include "board.hpp"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"

namespace {
constexpr unsigned ring_bits = 15;
alignas(1u << ring_bits) uint32_t pattern[(1u << ring_bits) / sizeof(uint32_t)];
}
void start_pattern_source(PIO pio, unsigned base) {
    // 65,536 chronological nibbles; see bench.py for the independent checker.
    uint16_t state = 1;
    for (auto& word : pattern) {
        uint32_t value = 0;
        for (unsigned nib = 0; nib < 8; ++nib) {
            value |= uint32_t(state & 15) << (4 * nib);
            for (unsigned bit = 0; bit < 4; ++bit)
                state = (state >> 1) ^ ((state & 1) ? 0xb400u : 0u);
        }
        word = value;
    }
    unsigned sm = pio_claim_unused_sm(pio, true);
    const uint16_t instructions[] = {
        uint16_t(pio_encode_out(pio_pins, 4) | pio_encode_sideset(1, 0)),
        uint16_t(pio_encode_nop() | pio_encode_sideset(1, 1)),
    };
    const pio_program_t program{instructions, 2, -1};
    unsigned offset = pio_add_program(pio, &program);
    for (unsigned p = base; p <= base + 4; ++p) pio_gpio_init(pio, p);
    pio_sm_config cfg = pio_get_default_sm_config();
    sm_config_set_wrap(&cfg, offset, offset + 1);
    sm_config_set_out_pins(&cfg, base, 4);
    sm_config_set_sideset(&cfg, 1, false, false);
    sm_config_set_sideset_pins(&cfg, base + 4);
    sm_config_set_out_shift(&cfg, true, true, 32);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&cfg, float(clock_get_hz(clk_sys)) / (2 * board::sample_hz));
    pio_sm_set_consecutive_pindirs(pio, sm, base, 5, true);
    pio_sm_init(pio, sm, offset, &cfg);
    unsigned ch = dma_claim_unused_channel(true);
    auto dc = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, true));
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_ring(&dc, false, ring_bits);
    dma_channel_configure(ch, &dc, &pio->txf[sm], pattern, dma_encode_endless_transfer_count(), true);
    pio_sm_set_enabled(pio, sm, true);
}

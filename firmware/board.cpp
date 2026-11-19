// SPDX-License-Identifier: Apache-2.0
#include "board.hpp"
#include "registers.hpp"
#include "hardware/spi.h"
#include "pico/stdlib.h"

namespace board {
void configure_frontend() {
    gpio_init(select_pin);
    gpio_put(select_pin, true);
    gpio_set_dir(select_pin, GPIO_OUT);
    spi_init(spi0, 1000000);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(sclk_pin, GPIO_FUNC_SPI);
    gpio_set_function(sdata_pin, GPIO_FUNC_SPI);
    sleep_ms(5);  // TCXO startup, measured from a valid receiver supply.
    for (unsigned address = 0; address < register_data.size(); ++address) {
        auto bytes = configuration_bytes(address);
        gpio_put(select_pin, false);
        busy_wait_us_32(1);
        spi_write_blocking(spi0, bytes.data(), bytes.size());
        while (spi_is_busy(spi0)) tight_loop_contents();
        gpio_put(select_pin, true);
        busy_wait_us_32(2);
    }
}
}

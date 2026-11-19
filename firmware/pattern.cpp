// SPDX-License-Identifier: Apache-2.0
#include "board.hpp"
#include "pattern_source.hpp"
#include "pico/stdlib.h"

int main() {
    start_pattern_source(pio0, board::data_base);
    for (;;) tight_loop_contents();
}

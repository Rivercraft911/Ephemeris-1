// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "hardware/pio.h"
// Starts a continuous 4-bit PRBS fixture on base..base+3 and a clock on base+4.
void start_pattern_source(PIO pio, unsigned base);

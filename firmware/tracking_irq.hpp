// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "receiver/app.hpp"

namespace board {
#ifdef EPHEMERIS_DUAL_TRACKING
void install_tracking_irq(ephemeris::Receiver&);
int core0_stack_untouched();
inline constexpr unsigned tracking_cores = 2;
#else
inline void install_tracking_irq(ephemeris::Receiver&) {}
inline int core0_stack_untouched() { return -1; }
inline constexpr unsigned tracking_cores = 1;
#endif
}

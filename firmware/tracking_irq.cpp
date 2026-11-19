// SPDX-License-Identifier: Apache-2.0
#include "tracking_irq.hpp"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include <atomic>
#include <cstdint>

extern "C" uint32_t __StackBottom;

namespace board {
namespace {
ephemeris::Receiver* receiver;
unsigned bell;
constexpr uint32_t stack_mark = 0x83b71e49;
volatile uint32_t* marked_start;
volatile uint32_t* marked_end;
std::atomic<bool> inspect_stack{false};
int untouched;

void mark_spare_stack() {
    const auto irq_state = save_and_disable_interrupts();
    uintptr_t sp;
    asm volatile("mov %0,sp" : "=r"(sp));
    // The hardware guard owns the bottom 32 bytes. Leave ample room below
    // this live frame. Fill only unused SRAM with other interrupts disabled.
    marked_start = reinterpret_cast<uint32_t*>(uintptr_t(&__StackBottom)+32);
    marked_end = reinterpret_cast<uint32_t*>((sp-256u) & ~uintptr_t(3));
    for (auto* p=marked_start; p<marked_end; ++p) *p = stack_mark;
    restore_interrupts(irq_state);
}

void tracking_irq() {
    if (!multicore_doorbell_is_set_current_core(bell)) return;
    multicore_doorbell_clear_current_core(bell);
    receiver->work_tracking();
    if (inspect_stack.load(std::memory_order_acquire)) {
        auto* p = marked_start;
        while (p < marked_end && *p == stack_mark) ++p;
        untouched = int(uintptr_t(p)-uintptr_t(marked_start));
        inspect_stack.store(false, std::memory_order_release);
    }
    __dsb();  // Complete the SRAM write before issuing a non-memory wake event.
    __sev();  // Wake an owner waiting for the release-store on the completed job.
}
void notify(void*) {
    __dmb();  // Publish the normal-memory job before ringing the device register.
    multicore_doorbell_set_other_core(bell);
}
void wait(void*) { __wfe(); }
}
void install_tracking_irq(ephemeris::Receiver& rx) {
    // Only core 0 receives this doorbell. Unlike the FIFO, it remains entirely
    // separate from the SDK's core-launch handshake. Core 1 owns process().
    receiver = &rx;
    mark_spare_stack();
    bell = multicore_doorbell_claim_unused(1, true);
    multicore_doorbell_clear_current_core(bell);
    const unsigned irq = multicore_doorbell_irq_num(bell);
    irq_set_exclusive_handler(irq, tracking_irq);
    irq_set_enabled(irq, true);
    rx.set_tracking_worker(notify, wait, nullptr);
}
int core0_stack_untouched() {
    // Request this only after completing the timed run. The interrupt reads
    // its own core's stack while foreground acquisition is suspended.
    inspect_stack.store(true, std::memory_order_release);
    notify(nullptr);
    while (inspect_stack.load(std::memory_order_acquire)) __wfe();
    return untouched;
}
}

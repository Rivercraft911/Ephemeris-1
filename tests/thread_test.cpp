// SPDX-License-Identifier: Apache-2.0
#include "receiver/app.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace sig = eph::signal;
namespace {
using Packed = std::array<uint8_t, sig::bytes_per_ms>;
Packed strong_prn_one() {
    sig::Code code{};
    assert(sig::ca_code(1, code));
    Packed packed{};
    constexpr int ci[] = {1,-1,-1,1}, cq[] = {1,1,-1,-1};
    for (unsigned n = 0; n < sig::raw_per_ms; ++n) {
        const int chip = code[n/16];
        const unsigned nibble = (ci[n%4]*chip < 0 ? 13u : 5u) | (cq[n%4]*chip < 0 ? 2u : 0u);
        packed[n/2] |= nibble << (4*(n%2));
    }
    return packed;
}

// Drive the real search worker on a different thread. Each barrier drains all
// available search work before the owner chooses to publish the next epoch.
class Worker {
public:
    explicit Worker(ephemeris::Receiver& rx) : thread_([this,&rx] {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            changed_.wait(lock, [this]{ return run_ || stop_; });
            if (stop_) break;
            lock.unlock();
            while (rx.work_acquisition()) {}
            lock.lock();
            run_ = false;
            changed_.notify_one();
        }
    }) {}
    void drain() {
        std::unique_lock<std::mutex> lock(mutex_);
        run_ = true;
        changed_.notify_one();
        changed_.wait(lock, [this]{ return !run_; });
    }
    ~Worker() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        changed_.notify_one();
        thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool run_ = false, stop_ = false;
    std::thread thread_;
};

enum class Handoff { current, gap, expired };
constexpr uint64_t origin = (uint64_t{1} << 32) - 5*sig::raw_per_ms;
void handoff(const Packed& packed, Handoff condition) {
    auto rx = std::make_unique<ephemeris::Receiver>();
    Worker worker(*rx);
    unsigned epoch = 0;
    auto publish = [&](bool gap = false) {
        rx->process(packed.data(), origin + uint64_t(epoch++)*sig::raw_per_ms, gap);
    };
    // Four disjoint search dwells followed by four fresh confirmation dwells.
    for (unsigned i = 0; i < 7; ++i) { publish(); worker.drain(); }
    publish();
    if (condition == Handoff::expired) {
        // The eighth snapshot remains claimed while capture advances. Only
        // release the worker when the queued snapshot will be exactly 250 ms
        // old at the next owner epoch.
        for (unsigned i = 0; i < 249; ++i) publish();
    }
    worker.drain();  // confirmed detection is queued; owner has not consumed it
    assert(rx->status().active_mask == 0);
    publish(condition == Handoff::gap);
    assert(rx->status().active_mask == (condition == Handoff::current ? 1u : 0u));
    assert(rx->status().gaps == (condition == Handoff::gap ? 1u : 0u));
    assert(!rx->status().fix.valid);
}

void concurrent_resets(const Packed& packed) {
    auto rx = std::make_unique<ephemeris::Receiver>();
    std::atomic<bool> done{false};
    std::thread worker([&] {
        while (!done.load(std::memory_order_acquire)) {
            if (!rx->work_acquisition()) std::this_thread::yield();
        }
    });
    for (unsigned i = 0; i < 1024; ++i) {
        const bool gap = i%17 == 0;
        rx->process(packed.data(), origin + uint64_t(i)*sig::raw_per_ms, gap);
        if (gap) assert(!rx->status().active_mask && !rx->status().fix.valid);
    }
    done.store(true, std::memory_order_release);
    worker.join();
}
}
int main() {
    const auto packed = strong_prn_one();
    for (auto mode : {Handoff::current, Handoff::gap, Handoff::expired}) handoff(packed, mode);
    concurrent_resets(packed);
}

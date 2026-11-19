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

void parallel_tracking() {
    auto serial = std::make_unique<ephemeris::Receiver>();
    auto dual = std::make_unique<ephemeris::Receiver>();
    struct Dispatch { std::mutex mutex; std::condition_variable wake; bool job{}, stop{}; } queue;
    dual->set_tracking_worker([](void* context) {
        auto& q = *static_cast<Dispatch*>(context);
        std::lock_guard<std::mutex> lock(q.mutex);
        q.job = true;
        q.wake.notify_one();
    }, nullptr, &queue);
    unsigned jobs = 0;
    // The physical doorbell can interrupt a transform at any instruction.
    // Exercise overlapping search and tracking calls under native TSan. Stay
    // below one complete cold sweep so search cannot introduce extra seeds.
    std::atomic<bool> search_done{false};
    std::thread search([&] {
        unsigned steps = 0;
        while (steps < 300 && !search_done.load(std::memory_order_acquire)) {
            if (dual->work_acquisition()) ++steps;
            else std::this_thread::yield();
        }
    });
    std::thread worker([&] {
        std::unique_lock<std::mutex> lock(queue.mutex);
        for (;;) {
            queue.wake.wait(lock, [&]{ return queue.job || queue.stop; });
            if (queue.stop) break;
            queue.job = false;
            lock.unlock();
            assert(dual->work_tracking());
            ++jobs;
            lock.lock();
        }
    });
    std::array<sig::Code, 8> codes;
    for (unsigned i = 0; i < codes.size(); ++i) assert(sig::ca_code(i+1, codes[i]));
    Packed input{};
    for (unsigned n = 0; n < sig::raw_per_ms; ++n) {
        int amplitude = 0;
        for (unsigned i = 0; i < codes.size(); ++i)
            amplitude += codes[i][(n/16+37*i)%sig::code_length];
        const int re[] = {amplitude,-amplitude,-amplitude,amplitude};
        const int im[] = {amplitude,amplitude,-amplitude,-amplitude};
        auto adc = [](int x) { return (x < 0 ? 2u : 0u) | (std::abs(x) > 1 ? 1u : 0u); };
        unsigned nibble = (adc(re[n%4]) << 2) | adc(im[n%4]);
        input[n/2] |= nibble << (4*(n%2));
    }
    unsigned epoch = 0;
    for (unsigned channels : {1u, 4u, 6u, 8u}) {
        const auto first = uint64_t(epoch++)*sig::raw_per_ms;
        for (auto* receiver : {serial.get(), dual.get()}) {
            receiver->process(input.data(), first, true);
            for (unsigned i = 0; i < channels; ++i) {
                sig::Detection hit{};
                hit.prn = i+1; hit.detected = true; hit.first_sample = first;
                hit.code_phase_chips = 37*i;
                assert(receiver->seed_channel(hit));
            }
        }
        for (unsigned n = 0; n < 220; ++n, ++epoch) {
            const uint64_t at = uint64_t(epoch)*sig::raw_per_ms;
            serial->process(input.data(), at, false);
            dual->process(input.data(), at, false);
            assert(dual->status().locked_mask == serial->status().locked_mask);
            assert(dual->status().active_mask == serial->status().active_mask);
            assert(dual->status().sequence == serial->status().sequence);
            for (unsigned i = 0; i < channels; ++i) {
                const auto& a = serial->channel(i);
                const auto& b = dual->channel(i);
                assert(a.prn == b.prn && a.code_phase_chips == b.code_phase_chips &&
                       a.doppler_hz == b.doppler_hz && a.code_epochs == b.code_epochs &&
                       a.prompt_i == b.prompt_i && a.prompt_q == b.prompt_q && a.count == b.count);
                for (unsigned j = 0; j < a.count; ++j)
                    assert(a.prompts[j].start_sample == b.prompts[j].start_sample &&
                           a.prompts[j].end_sample == b.prompts[j].end_sample);
            }
        }
        assert(dual->status().locked_mask == ((1u<<channels)-1));
    }
    { std::lock_guard<std::mutex> lock(queue.mutex); queue.stop = true; }
    queue.wake.notify_one();
    worker.join();
    search_done.store(true, std::memory_order_release);
    search.join();
    assert(jobs == 660);
}
}
int main() {
    const auto packed = strong_prn_one();
    for (auto mode : {Handoff::current, Handoff::gap, Handoff::expired}) handoff(packed, mode);
    concurrent_resets(packed);
    parallel_tracking();
}

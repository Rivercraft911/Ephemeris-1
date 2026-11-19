// SPDX-License-Identifier: Apache-2.0
// Host-paced, cold EIQ replay. Core 0 validates USB frames while core 1
// advances the receiver in recording time, with a fixed acquisition budget.
#include "usb.hpp"
#include "receiver/app.hpp"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>

namespace {
namespace sig = eph::signal;
constexpr unsigned max_epochs = 4, window = 2;
constexpr uint64_t timeout_us = 30000000;
struct Frame {
    uint8_t iq[max_epochs * sig::bytes_per_ms];
    uint32_t seq, count, crc, work, steps, tracking_us, acquisition_us;
    uint64_t first;
    char status[480]; // at most one 200 ms report per four-epoch frame
};
Frame frames[window];
alignas(32) uint32_t worker_stack[4096];
ephemeris::Receiver receiver;
std::atomic<uint32_t> submitted{0}, completed{0};
uint32_t reported = 0;

constexpr std::array<uint32_t,256> crc_table() {
    std::array<uint32_t,256> t{};
    for (uint32_t i=0; i<256; ++i) {
        uint32_t v=i;
        for (unsigned b=0;b<8;++b) v=(v>>1)^((0u-(v&1u)) & 0xedb88320u);
        t[i]=v;
    }
    return t;
}
constexpr auto table=crc_table();
uint32_t update_crc(uint32_t v,const uint8_t* data,size_t size) {
    for (size_t i=0;i<size;++i) v=(v>>8)^table[(v^data[i])&255];
    return v;
}
uint64_t little(const uint8_t* p,unsigned size) {
    uint64_t value=0;
    for (unsigned i=0;i<size;++i) value|=uint64_t(p[i])<<(8*i);
    return value;
}
bool send(const char* text) {
    const auto n=std::strlen(text);
    size_t pos=0;
    auto last=time_us_64();
    while (pos<n) {
        usb::poll();
        if (!usb::connected() || time_us_64()-last>timeout_us) return false;
        const auto written=usb::write(text+pos,n-pos);
        if (written) { pos+=written; last=time_us_64(); }
    }
    return true;
}
void worker() {
    uint32_t consumed=0;
    for (;;) {
        if (consumed==submitted.load(std::memory_order_acquire)) {
            tight_loop_contents();
            continue;
        }
        auto& f=frames[consumed%window];
        f.steps=f.tracking_us=f.acquisition_us=0;
        f.status[0]=0;
        for (unsigned i=0;i<f.count;++i) {
            const auto previous=receiver.status().sequence;
            const auto start=time_us_32();
            receiver.process(f.iq+i*sig::bytes_per_ms,f.first+uint64_t(i)*sig::raw_per_ms,false);
            const auto middle=time_us_32();
            for (unsigned j=0;j<f.work && receiver.work_acquisition();++j) ++f.steps;
            f.tracking_us+=middle-start;
            f.acquisition_us+=time_us_32()-middle;
            if (previous!=receiver.status().sequence)
                ephemeris::format_status(f.status,sizeof(f.status),receiver.status());
        }
        completed.store(++consumed,std::memory_order_release);
    }
}
bool report_completed() {
    while (reported!=completed.load(std::memory_order_acquire)) {
        const auto& f=frames[reported%window];
        if (f.status[0] && !(send("S ") && send(f.status) && send("\n"))) return false;
        char msg[144];
        std::snprintf(msg,sizeof(msg),"A %" PRIu32 " %" PRIu64 " %" PRIu32 " %08" PRIx32
                      " %" PRIu32 " %" PRIu32 " %" PRIu32 "\n",f.seq,f.first,f.count,
                      f.crc,f.steps,f.tracking_us,f.acquisition_us);
        if (!send(msg)) return false;
        ++reported;
    }
    return usb::connected();
}
bool receive(uint8_t* dst,size_t size) {
    size_t pos=0;
    auto last=time_us_64();
    while (pos<size) {
        usb::poll();
        if (!report_completed() || time_us_64()-last>timeout_us) return false;
        const auto received=usb::read(dst+pos,size-pos);
        if (received) {pos+=received; last=time_us_64();}
    }
    return true;
}
void session(unsigned work) {
    // Prior frames may still be finishing after a detached host. Wait for
    // core 1 to release all receiver/slot state before a genuine cold reset.
    while (submitted.load()!=completed.load(std::memory_order_acquire)) usb::poll();
    reported=completed.load();
    receiver.~Receiver();
    new(&receiver) ephemeris::Receiver{};
    char banner[100];
    std::snprintf(banner,sizeof(banner),"READY EPR1 %u %u %u\n",max_epochs,window,work);
    if (!(send(banner) && send("H ") && send(ephemeris::status_header) && send("\n"))) return;
    uint32_t expected=0;
    uint64_t next=0;
    for (;;) {
        uint8_t head[24];
        if (!receive(head,sizeof(head))) {send("ERR short header or timeout\n"); return;}
        const unsigned count=little(head+16,2);
        const uint64_t first=little(head+8,8);
        if (std::memcmp(head,"EPB1",4) || little(head+4,4)!=expected || little(head+18,2)!=0 ||
            count>max_epochs || (expected && first!=next) || first>UINT64_MAX-uint64_t(count)*sig::raw_per_ms) {
            send("ERR bad frame sequence or interval\n"); return;
        }
        const auto issued=submitted.load(std::memory_order_relaxed);
        while (issued-reported>=window) {
            usb::poll();
            if (!report_completed()) return;
        }
        auto& f=frames[issued%window];
        const auto bytes=count*sig::bytes_per_ms;
        if (bytes && !receive(f.iq,bytes)) {send("ERR short payload or timeout\n"); return;}
        f.crc=~update_crc(update_crc(~uint32_t{},head,20),f.iq,bytes);
        if (f.crc!=little(head+20,4)) {send("ERR frame CRC\n"); return;}
        if (!count) {
            while (reported!=issued) {usb::poll(); if (!report_completed()) return;}
            char done[88];
            std::snprintf(done,sizeof(done),"DONE %" PRIu32 " %" PRIu64 "\n",expected,first);
            send(done); return;
        }
        f.seq=expected++; f.first=first; f.count=count; f.work=work;
        next=first+uint64_t(count)*sig::raw_per_ms;
        submitted.store(issued+1,std::memory_order_release);
    }
}
}
int main() {
    usb::init();
    multicore_launch_core1_with_stack(worker,worker_stack,sizeof(worker_stack));
    for (;;) {
        usb::poll();
        char cmd[40], extra;
        unsigned work=0;
        if (!usb::command(cmd,sizeof(cmd))) continue;
        if (std::sscanf(cmd,"replay %u%c",&work,&extra)==1 && work>0 && work<=128) session(work);
        else send("ERR expected replay 1..128 or bootsel\n");
    }
}

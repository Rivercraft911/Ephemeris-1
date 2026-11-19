// SPDX-License-Identifier: Apache-2.0
#include "receiver/app.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
uint64_t little(const uint8_t* p, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
int run(int argc, char** argv) {
    if (argc < 2) throw std::runtime_error("usage: eph-replay INPUT.eiq [--work bins-per-ms] [--channels file.csv] [--warm-reset-ms ms] [--seed PRN,DOPPLER_HZ,PHASE_CHIPS]");
    std::ifstream file;
    if (std::strcmp(argv[1], "-") != 0) file.open(argv[1], std::ios::binary);
    std::istream& input = std::strcmp(argv[1], "-") == 0 ? std::cin : file;
    std::array<uint8_t, 32> hdr{};
    if (!input.read(reinterpret_cast<char*>(hdr.data()), hdr.size()) ||
        std::memcmp(hdr.data(), "EIQ1", 4) || little(hdr.data()+4,4) != 16368000 ||
        little(hdr.data()+8,4) != 4092000 || little(hdr.data()+12,4) != 1)
        throw std::runtime_error("invalid EIQ1 header or unsupported sampling convention");
    const uint64_t start = little(hdr.data()+16,8), count = little(hdr.data()+24,8);
    if (!count || count % eph::signal::raw_per_ms || count > UINT64_MAX - start)
        throw std::runtime_error("invalid complete-millisecond sample interval");
    auto rx = std::make_unique<ephemeris::Receiver>();
    std::ofstream channels;
    unsigned work = 8;
    uint64_t reset_ms = 0;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (++i == argc) throw std::runtime_error("missing option value for " + arg);
        if (arg == "--work") {
            const int parsed = std::stoi(argv[i]);
            if (parsed < 0 || parsed > 4096) throw std::runtime_error("work must be in [0,4096]");
            work = unsigned(parsed);
        } else if (arg == "--warm-reset-ms") {
            size_t consumed = 0;
            reset_ms = std::stoull(argv[i], &consumed);
            if (argv[i][consumed] || !reset_ms || reset_ms >= count / eph::signal::raw_per_ms)
                throw std::runtime_error("warm reset must fall inside the recording");
        } else if (arg == "--channels") {
            channels.open(argv[i]);
            if (!channels) throw std::runtime_error("cannot open channel output");
            channels << "sample,prn,locked,doppler_hz,code_phase_chips,i,q,subframes,sv_tow_at_block_end_s\n";
            channels.precision(16);
        } else if (arg == "--seed") {
            eph::signal::Detection seed{};
            char trailing;
            if (std::sscanf(argv[i], "%u,%f,%lf%c", &seed.prn, &seed.doppler_hz, &seed.code_phase_chips, &trailing) != 3)
                throw std::runtime_error("invalid lab seed");
            seed.first_sample = start; seed.detected = true;
            if (!rx->seed_channel(seed)) throw std::runtime_error("seed rejected");
        } else throw std::runtime_error("unknown option " + arg);
    }
    std::array<uint8_t, eph::signal::bytes_per_ms> bytes{};
    char row[512];
    unsigned sequence = 0;
    std::cout << ephemeris::status_header << '\n';
    using Clock = std::chrono::steady_clock;
    double worst_us = 0;
    const auto begin = Clock::now();
    for (uint64_t offset = 0; offset < count; offset += eph::signal::raw_per_ms) {
        if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            throw std::runtime_error("truncated capture");
        if (reset_ms && offset / eph::signal::raw_per_ms == reset_ms) {
            ephemeris::WarmState saved;
            rx->save_warm(saved);
            rx = std::make_unique<ephemeris::Receiver>();
            if (!rx->restore_warm(saved)) throw std::runtime_error("warm restore failed");
            sequence = 0;
            std::cerr << "warm reset at sample " << start + offset << '\n';
        }
        const auto tick = Clock::now();
        rx->process(bytes.data(), start + offset, false);
        const double us = std::chrono::duration<double, std::micro>(Clock::now()-tick).count();
        worst_us = std::max(worst_us, us);
        for (unsigned j = 0; j < work && rx->work_acquisition(); ++j) {}
        if (channels.is_open()) {
            for (unsigned j = 0; j < rx->channels; ++j) {
                const auto& tr = rx->channel(j);
                if (!tr.active) continue;
                double tx = 0;
                const bool timed = rx->channel_tx_tow(j, double(tr.first_sample + eph::signal::raw_per_ms), tx);
                channels << tr.first_sample << ',' << tr.prn << ',' << tr.locked << ','
                    << tr.doppler_hz << ',' << tr.code_phase_chips << ',' << tr.prompt_i << ','
                    << tr.prompt_q << ',' << rx->subframes(j) << ',';
                if (timed) channels << tx;
                channels << '\n';
            }
        }
        if (sequence != rx->status().sequence) {
            sequence = rx->status().sequence;
            ephemeris::format_status(row,sizeof(row),rx->status());
            std::cout << row << '\n';
        }
    }
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("capture has trailing bytes");
    std::cerr << "native replay: " << count / eph::signal::raw_per_ms << " ms, elapsed "
              << std::chrono::duration<double>(Clock::now()-begin).count() << " s, worst processing "
              << worst_us << " us (acquisition timed separately from processing)\n";
    return 0;
}
}
int main(int argc, char** argv) {
    try { return run(argc,argv); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

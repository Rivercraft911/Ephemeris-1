// SPDX-License-Identifier: Apache-2.0
// Test adapter for the production PVT solver. Input consists only of broadcast
// elements, receiver clock tags and synthetic radio observables; truth stays in Python.
#include "receiver/navigation.hpp"
#include <array>
#include <iomanip>
#include <iostream>

int main() {
    namespace nav = eph::nav;
    std::array<nav::Ephemeris,32> broadcast{};
    unsigned nsv;
    if (!(std::cin >> nsv) || nsv > 32) return 2;
    for (unsigned i=0; i<nsv; ++i) {
        auto& e=broadcast[i];
        if (!(std::cin >> e.week_mod1024 >> e.toe >> e.sqrt_a >> e.i0 >> e.omega0 >> e.m0
                  >> e.af0 >> e.af1)) return 2;
        e.health = e.ura = e.iodc = 0;
        e.sf1_tow = e.toc = e.toe;
    }
    double local;
    unsigned count;
    std::cout << std::setprecision(14);
    while (std::cin >> local >> count) {
        if (count > nsv) return 2;
        std::array<nav::Observation,32> obs{};
        for (unsigned i=0; i<count; ++i) {
            auto& o=obs[i];
            if (!(std::cin >> o.prn >> o.sv_tow_s >> o.doppler_hz) || o.prn<1 || o.prn>nsv) return 2;
            o.ephemeris = &broadcast[o.prn-1];
        }
        nav::Fix f;
        nav::solve_fix(obs.data(),count,f,local);
        std::cout << f.valid;
        for (double x : f.position_m) std::cout << ' ' << x;
        for (double v : f.velocity_mps) std::cout << ' ' << v;
        std::cout << ' ' << f.tow_s << ' ' << f.satellites << '\n';
    }
    return std::cin.eof() ? 0 : 2;
}

// SPDX-License-Identifier: GPL-3.0-only
// Network stream content rate (engine/StreamArrivalRate.h) and the adaptive
// multiplier it drives for PS5 Remote Play (engine/AdaptiveFgMultiplier.h).
#include "veyra/engine/StreamArrivalRate.h"
#include "veyra/engine/AdaptiveFgMultiplier.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace veyra::engine;
static int failures = 0;
static void check(bool ok, const std::string& label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}
static bool near(double value, double want, double tolerance) { return std::abs(value - want) <= tolerance; }

namespace {
// A stream as the engine sees it: the source numbers every decoded frame, and each
// arrives at the console's frame time plus network jitter (deterministic, up to
// +-jitter, never reordered). Times in 100 ns.
struct Stream {
    StreamArrivalRate rate;
    uint64_t sequence = 0;
    double clock = 0;
    int64_t jitter = 0, last = 0;
    uint32_t seed = 12345;
    int64_t frame(double fps, bool read = true) {
        clock += 1e7 / fps;
        ++sequence;
        seed = seed * 1664525u + 1013904223u;
        const double unit = double(seed >> 8) / double(1u << 24) * 2 - 1;
        last = std::max(last, int64_t(clock) + int64_t(unit * double(jitter)));
        if (read) rate.observe(sequence, last);
        return last;
    }
    void pause(double seconds) { clock += seconds * 1e7; }
};
}

int main() {
    using B = FrameGenerationBackend;
    {
        Stream s;
        for (int i = 0; i < 15; ++i) s.frame(30);
        check(s.rate.fps() == 0, "no rate before half a second (14 intervals)");
        s.frame(30); s.frame(30);
        check(near(s.rate.fps(), 30, 0.01), "30 FPS once half a second is covered (16 intervals)");
    }
    {
        // Steady 30 FPS with +-8 ms network jitter for a minute.
        Stream s; s.jitter = 80000;
        double lo = 1e9, hi = 0; bool twoX = true;
        for (int i = 0; i < 1800; ++i) {
            s.frame(30);
            const double fps = s.rate.fps();
            if (i < 30) continue;
            lo = std::min(lo, fps); hi = std::max(hi, fps);
            if (adaptiveFgMultiplier(B::Vfg, 3, fps, 70) != 2) twoX = false;
        }
        check(lo >= 29.4 && hi <= 30.6, "steady 30 FPS with 8 ms jitter stays within 29.4-30.6 (got " + std::to_string(lo) + "-" + std::to_string(hi) + ")");
        check(twoX, "steady 30 FPS never leaves 2X at a 70 FPS cap");
    }
    {
        // The engine misses frames (latest-frame mailbox): the source numbers still count them.
        Stream s, t;
        for (int i = 0; i < 300; ++i) { s.frame(30, i % 3 != 2); t.frame(30, i % 4 == 0); }
        check(near(s.rate.fps(), 30, 0.01), "a third of the frames unread: still 30 FPS");
        check(near(t.rate.fps(), 30, 0.01), "three of four frames unread: still 30 FPS");
    }
    {
        // A dip to 25 FPS shows within a second.
        Stream s;
        for (int i = 0; i < 90; ++i) s.frame(30);
        int64_t start = s.last, crossed = 0;
        for (int i = 0; i < 25; ++i) {
            const auto at = s.frame(25);
            if (!crossed && s.rate.fps() < 28.5) crossed = at;
        }
        check(near(s.rate.fps(), 25, 0.05), "one second into a 25 FPS dip the rate is 25");
        check(crossed && crossed - start <= 6000000, "the dip is visible within 0.6 s");
    }
    {
        Stream s;
        for (int i = 0; i < 60; ++i) s.frame(30);
        const double before = s.rate.fps();
        s.rate.observe(s.sequence, s.last + 100000);
        check(s.rate.fps() == before, "the same frame read again changes nothing");
        s.rate.observe(0, s.last + 200000);
        check(s.rate.fps() == before, "an unnumbered frame is ignored");
        s.rate.observe(1, s.last + 333333);
        check(s.rate.fps() == 0, "a renumbered source starts over");
        Stream c;
        for (int i = 0; i < 60; ++i) c.frame(30);
        c.rate.observe(c.sequence + 1, c.last - 10000000);
        check(c.rate.fps() == 0, "a clock step back starts over");
    }
    {
        // A two second stall is not averaged into the rate.
        Stream s;
        for (int i = 0; i < 60; ++i) s.frame(30);
        s.pause(2);
        s.frame(30);
        check(s.rate.fps() == 0, "right after a 2 s stall there is no rate");
        for (int i = 0; i < 16; ++i) s.frame(30);
        check(near(s.rate.fps(), 30, 0.01), "half a second after the stall: 30 FPS again");
    }
    {
        // Storage stays bounded; a full buffer still gives a rate.
        Stream s;
        for (int i = 0; i < 4000; ++i) s.frame(2000);
        check(near(s.rate.fps(), 2000, 1), "2000 FPS fills the buffer and still measures");
    }

    // The field case (2026-11-20): GTA on a PS5 30 FPS profile, NVIDIA VFG, ceiling 3X,
    // output cap 70. 30 FPS should run at 2X (60), a dip below ~28.6 at 3X (capped 70).
    check(adaptiveFgMultiplier(B::Vfg, 3, 30, 70) == 2, "VFG 3X ceiling at a 70 cap: 30 FPS -> 2X");
    check(adaptiveFgMultiplier(B::Vfg, 3, 29, 70) == 2, "29 FPS -> 2X (58 beats 87)");
    check(adaptiveFgMultiplier(B::Vfg, 3, 28, 70) == 3, "28 FPS -> 3X (84 beats 56)");
    check(adaptiveFgMultiplier(B::Vfg, 3, 25, 70) == 3, "25 FPS -> 3X");
    {
        Stream s; s.jitter = 50000;
        AdaptiveFgGate gate;
        unsigned changes = 0, atEnd[3] = {};
        int64_t phaseStart[3] = {}, switchedAt[3] = {};
        const double rates[3] = {30, 25, 30};
        for (int phase = 0; phase < 3; ++phase) {
            phaseStart[phase] = s.last;
            for (int i = 0; i < int(rates[phase] * 4); ++i) {
                const auto at = s.frame(rates[phase]);
                const double fps = s.rate.fps();
                if (fps <= 0) continue;
                bool changed = false;
                gate.update(adaptiveFgMultiplier(B::Vfg, 3, fps, 70), at, &changed);
                if (changed) { ++changes; if (!switchedAt[phase]) switchedAt[phase] = at; }
            }
            atEnd[phase] = gate.applied();
        }
        check(atEnd[0] == 2 && atEnd[1] == 3 && atEnd[2] == 2, "30 -> 25 -> 30 FPS runs 2X, 3X, 2X");
        check(changes == 3, "one first choice and two switches, no flicker (got " + std::to_string(changes) + ")");
        check(switchedAt[1] && switchedAt[1] - phaseStart[1] <= 15000000, "3X within 1.5 s of the dip");
        check(switchedAt[2] && switchedAt[2] - phaseStart[2] <= 15000000, "back to 2X within 1.5 s of recovery");
    }

    std::printf("%s\n", failures ? "StreamArrivalRate tests FAILED" : "StreamArrivalRate tests passed");
    return failures ? 1 : 0;
}

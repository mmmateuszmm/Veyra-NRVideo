// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace veyra::engine {
// Decoded-frame rate of a network stream (PS5 Remote Play, Moonlight, Xbox), the
// content rate of the adaptive frame generation multiplier (AdaptiveFgMultiplier.h)
// for these sources.
//
// A stream's SourceInfo::averageFps is the profile the client asked for: a PS5
// 30 FPS profile reports 30 whatever the game does. Its frames are hardware
// decoded, so duplicate removal has no CPU picture to compare and its rate stays
// 0 (ContentDedup "no-picture"). The console only sends new pictures, though: a
// game that dips to 25 FPS arrives as 25 frames a second, which is what the
// remoteplay-progress inputFps line shows (field log 2026-11-20, GTA on a 30 FPS
// profile: 20-29 during dips, 29-31 when steady).
//
// The rate is measured from the frames the engine reads. Every source numbers
// each decoded frame it publishes (FramePacket::sequence), so a frame the engine
// never read (latest-frame mailbox overwrite) still counts: an engine that falls
// behind does not lower the measured rate and raise its own load. Times are the
// frames' host arrival (FramePacket::arrivalHost100ns), not the read time, so
// engine scheduling jitter does not enter either.
//
// fps() = frames between the oldest and the newest observation of the last
// second / the time between them; 0 until that spans half a second and eight
// frames. A stall longer than the window starts the measurement again instead of
// averaging across it. Units: 100 ns.
class StreamArrivalRate {
public:
    static constexpr int64_t kWindow = 10000000;   // 1 s
    static constexpr int64_t kMinSpan = 5000000;   // 0.5 s
    static constexpr uint64_t kMinFrames = 8;
    static constexpr size_t kCapacity = 512;       // a full second up to 512 FPS

    void reset() { head_ = count_ = 0; }

    // sequence: the source's decoded-frame number (0 = unknown, ignored).
    // arrival: host time of that frame.
    void observe(uint64_t sequence, int64_t arrival) {
        if (sequence == 0) return;
        if (count_) {
            const auto& last = at(count_ - 1);
            if (sequence == last.sequence) return;                       // the same frame read again
            if (sequence < last.sequence || arrival < last.time) reset(); // renumbered source or clock step
        }
        if (count_ == kCapacity) drop();
        samples_[(head_ + count_) % kCapacity] = {arrival, sequence};
        ++count_;
        while (count_ > 1 && arrival - at(0).time > kWindow) drop();
    }

    double fps() const {
        if (count_ < 2) return 0.0;
        const auto& first = at(0);
        const auto& last = at(count_ - 1);
        const int64_t span = last.time - first.time;
        const uint64_t frames = last.sequence - first.sequence;
        if (span <= 0 || frames < kMinFrames || (span < kMinSpan && count_ < kCapacity)) return 0.0;
        return double(frames) * 1e7 / double(span);
    }

private:
    struct Sample { int64_t time = 0; uint64_t sequence = 0; };
    const Sample& at(size_t i) const { return samples_[(head_ + i) % kCapacity]; }
    void drop() { head_ = (head_ + 1) % kCapacity; --count_; }
    std::array<Sample, kCapacity> samples_{};
    size_t head_ = 0, count_ = 0;
};
}

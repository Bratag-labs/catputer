#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lofi {

// Resamples the 32 kHz music core to 48 kHz for the SDL preview output.
//
// The Cardputer Zero's pipewire-pulse 32k client path glitches: a 32 kHz
// tone played through it produced periodic full-scale clicks followed by
// ~140 ms of silence (measured 2026-11-21), while the same tone at
// 48 kHz — the sink's native rate — played clean, and the same 32 kHz tone
// through the direct ALSA path (aplay) was clean. The app's SDL audio goes
// through libpulse, so the preview opens its device at 48 kHz and resamples
// in-process. The firmware keeps 32 kHz: its I2S output needs no resample,
// and WAV exports stay 32 kHz.
//
// 32k -> 96k (3x zero-stuff) -> 48k (2x decimate) with a 25-tap Hamming
// windowed-sinc low-pass (20 kHz cutoff at 96 kHz, passband within 0.1 dB
// to 14 kHz).
//
// Two correctness details:
//
// 1. The decimation filter acts on the zero-stuffed signal, whose data
//    samples sit at every third position. A DC input therefore only
//    exercises coefficients at indices congruent to the phase mod 3, so
//    unity DC gain requires each of the three residue classes (mod 3) to
//    sum to 32768 individually — the table's total is 3 x 32768 (98302).
//    Per-phase sums land at 0.9982/1.00096/1.0006, a +/-0.1% ripple on
//    3 consecutive outputs, inaudible.
//
// 2. The filter is causal: 12 samples of group delay in the 96 kHz domain
//    (125 us). The last 12 outputs of a block need the first 24
//    zero-stuffed samples of the next block (its first 8 input samples),
//    which do not exist yet — so they are held and emitted at the start
//    of the next call. The first call returns 756 outputs; every later
//    call returns exactly 768 (12 held + 756). After warm-up the emitted
//    stream lags the input by a constant 12 outputs (250 us).
class OutputResampler {
public:
    static constexpr std::uint32_t kOutRate = 48000;
    static constexpr std::size_t kInBlock = 512;
    // Nominal outputs per call after warm-up (12 held + 756 emitted).
    static constexpr std::size_t kOutBlock = kInBlock * 3 / 2;
    static constexpr std::size_t kTaps = 25;
    // Outputs whose 25 taps lie entirely inside the current block: for
    // output i the taps span 96k positions 2i..2i+24, which fit under
    // kInBlock*3 - 1 (the last stored index) iff i <= (3*kInBlock - kTaps + 1)/2.
    static constexpr std::size_t kEmitted =
        (kInBlock * 3 - (kTaps - 1)) / 2;
    static constexpr std::size_t kPending = kOutBlock - kEmitted;
    // (kTaps - 1) history samples + one zero-stuffed block.
    static constexpr std::size_t kWindow = kInBlock * 3 + kTaps - 1;

    // Resamples kInBlock input samples. Returns kEmitted on the first call,
    // kOutBlock on every later call; output is written to out.
    std::size_t process(const std::int16_t* in, std::int16_t* out) {
        if (len_ + kInBlock * 3 > kWindow) {
            const std::size_t keep = kTaps - 1;
            std::memmove(buf_, buf_ + len_ - keep, keep * sizeof(std::int32_t));
            len_ = keep;
        }
        for (std::size_t i = 0; i < kInBlock; ++i) {
            buf_[len_++] = in[i];
            buf_[len_++] = 0;
            buf_[len_++] = 0;
        }
        const std::size_t base = len_ - kInBlock * 3;
        std::size_t o = 0;
        if (has_prev_) {
            // Flush the tail held from the previous block. Its output h
            // (h in 0..kPending) has taps at buf_[2*h .. 2*h + kTaps - 1]:
            // buf_[0..23] is the previous block's last 24 zero-stuffed
            // samples, so its taps are complete only now.
            for (std::size_t h = 0; h < kPending; ++h) {
                std::int64_t acc = 0;
                for (std::size_t t = 0; t < kTaps; ++t) {
                    acc += static_cast<std::int64_t>(buf_[2 * h + t]) * kCoeffs[t];
                }
                out[o++] = clamp_output((acc + 16384) >> 15);
            }
        }
        // This block's outputs whose taps fit inside it. For
        // i < kEmitted, base + 2*i + t <= base + kInBlock*3 - 2 < len_.
        for (std::size_t i = 0; i < kEmitted; ++i) {
            std::int64_t acc = 0;
            for (std::size_t t = 0; t < kTaps; ++t) {
                acc += static_cast<std::int64_t>(buf_[base + 2 * i + t]) * kCoeffs[t];
            }
            out[o++] = clamp_output((acc + 16384) >> 15);
        }
        has_prev_ = true;
        const std::size_t keep = kTaps - 1;
        std::memmove(buf_, buf_ + len_ - keep, keep * sizeof(std::int32_t));
        len_ = keep;
        return o;
    }

private:
    static std::int16_t clamp_output(std::int64_t v) {
        return static_cast<std::int16_t>(
            std::clamp(v, std::int64_t{-32768}, std::int64_t{32767}));
    }

    // Q15 windowed-sinc, fc = 20 kHz at 96 kHz, for the zero-stuffed
    // signal: each coefficient residue class mod 3 sums to 32768, so the
    // table's total is 3 x 32768 (98302). Max |coeff| is 40878; the
    // int64 accumulator keeps the worst-case sum in range.
    static const std::int32_t kCoeffs[kTaps];
    std::int32_t buf_[kWindow]{};
    std::size_t len_ = 0;
    bool has_prev_ = false;
};

const std::int32_t OutputResampler::kCoeffs[OutputResampler::kTaps] = {
    0, 262, 221, -527, -1048, 486, 2811, 1065, -5206, -6369, 7326, 29691,
    40878, 29691, 7326, -6369, -5206, 1065, 2811, 486, -1048, -527, 221,
    262, 0,
};

}  // namespace lofi

// 48 kHz preview output high-pass for the Cardputer Zero's built-in speaker.
//
// The speaker's cone has a few millimetres of usable travel; bass energy
// below its ~80-120 Hz resonance is barely audible on it but still drives
// the cone, and past the travel limit the driver rattles (user-confirmed
// crackle on bass, headphones clean, 2026-11-21). A gentle 2nd-order
// Butterworth high-pass removes the excursion demand below the cutoff.
//
// Second-order Butterworth biquad (Q = 1/sqrt(2)): -3 dB at the cutoff,
// 12 dB/octave below it. Streaming: the two pole/zero state pairs persist
// across process() calls, so it sits after the 32k->48k resampler on the
// queued preview output. The firmware and the 32 kHz WAV export are
// untouched.
#pragma once
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace lofi {

class HighPass {
public:
    // cutoffHz: -6 dB point; 0 = bypass (process() becomes a no-op).
    // sampleRate must match the caller's output rate (kOutRate = 48000).
    HighPass(double cutoffHz, double sampleRate) {
        if(cutoffHz == 0.0) { bypass_ = true; return; }
        if(cutoffHz <= 0.0 || cutoffHz > sampleRate / 2.0 || sampleRate <= 0.0)
            throw std::invalid_argument("HighPass: cutoffHz must be (0, sampleRate/2] or 0");
        const double w = 2.0 * M_PI * cutoffHz / sampleRate;
        const double c = std::cos(w);
        const double alpha = std::sin(w) / (2.0 * (1.0 / std::sqrt(2.0)));
        const double a0 = 1.0 + alpha;
        b0_ = (1.0 + c) * 0.5 / a0;
        b1_ = -(1.0 + c) / a0;
        b2_ = (1.0 + c) * 0.5 / a0;
        a1_ = -2.0 * c / a0;
        a2_ = (1.0 - alpha) / a0;
    }
    // Filter x in place; state persists across calls.
    void process(std::int16_t* x, std::size_t n) {
        if(bypass_) return;
        for(std::size_t i = 0; i < n; ++i) {
            const double in = x[i];
            const double out = b0_ * in + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
            x2_ = x1_; x1_ = in;
            y2_ = y1_; y1_ = out;
            x[i] = out >= 32767.0 ? 32767 : (out <= -32768.0 ? -32768 : static_cast<std::int16_t>(out));
        }
    }
private:
    double b0_ = 0, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
    bool bypass_ = false;
};

} // namespace lofi

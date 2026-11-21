// Unit tests for the 48 kHz preview output high-pass (native/high_pass.h).
// Deterministic: no audio device, no filesystem. Expected amplitudes follow
// the measured 75 Hz Butterworth response: 30 Hz -> 0.1586, 40 Hz -> 0.2746,
// 48 Hz -> 0.3797, 75 Hz -> 0.7071 (-3 dB cutoff), 150 Hz -> 0.9701,
// 1 kHz -> ~1 (all for a 10000-amplitude input).
#include "high_pass.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

constexpr double kRate = 48000.0;
constexpr std::size_t kN = 4800; // 100 ms
constexpr std::size_t kSettle = 1200; // skip ~25 ms of settling

std::array<std::int16_t, kN> tone(double f, double amp) {
    std::array<std::int16_t, kN> x{};
    for(std::size_t i = 0; i < kN; ++i)
        x[i] = static_cast<std::int16_t>(std::llround(amp * std::sin(2.0 * M_PI * f * i / kRate)));
    return x;
}

void check(bool ok, const std::string& what) {
    if(!ok) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
}

// Peak |y| over the settled window: for a steady-state sine this is the
// output amplitude = input amplitude times |H(f)|.
double settledPeak(const std::array<std::int16_t, kN>& y) {
    double p = 0;
    for(std::size_t i = kSettle; i < kN; ++i) p = std::max(p, static_cast<double>(std::abs(y[i])));
    return p;
}

double windowPeak(const std::array<std::int16_t, kN>& y, std::size_t from, std::size_t to) {
    double p = 0;
    for(std::size_t i = from; i < to; ++i) p = std::max(p, static_cast<double>(std::abs(y[i])));
    return p;
}

} // namespace

int main() {
    using lofi::HighPass;
    // -3 dB at the cutoff.
    {
        auto y = tone(75.0, 10000.0);
        HighPass hp(75.0, kRate);
        hp.process(y.data(), y.size());
        const double p = settledPeak(y);
        check(p > 6600 && p < 7500, "75 Hz at -3 dB cutoff, expected ~7071, got " + std::to_string(p));
    }
    // Unity in the passband.
    {
        auto y = tone(1000.0, 10000.0);
        HighPass hp(75.0, kRate);
        hp.process(y.data(), y.size());
        const double p = settledPeak(y);
        check(p > 9800 && p < 10100, "1 kHz passband ~unity, expected ~9984, got " + std::to_string(p));
    }
    // Sub-bass roll-off, monotone across the stopband.
    auto y30 = tone(30.0, 10000.0);
    auto y40 = tone(40.0, 10000.0);
    auto y48 = tone(48.0, 10000.0);
    auto y150 = tone(150.0, 10000.0);
    std::array<std::array<std::int16_t, kN>, 4> tones{y30, y40, y48, y150};
    for(auto& y : tones) { HighPass hp(75.0, kRate); hp.process(y.data(), y.size()); }
    const double p30 = settledPeak(tones[0]), p40 = settledPeak(tones[1]), p48 = settledPeak(tones[2]), p150 = settledPeak(tones[3]);
    check(p30 > 1400 && p30 < 1800, "30 Hz ~0.159 of input, got " + std::to_string(p30));
    check(p40 > 2500 && p40 < 3000, "40 Hz ~0.275 of input, got " + std::to_string(p40));
    check(p30 < p40 && p40 < p48 && p48 < 7071 && 7071 < p150,
          "monotone roll-off 30 < 40 < 48 < 75 < 150 Hz (" + std::to_string(p30) + "," +
              std::to_string(p40) + "," + std::to_string(p48) + "," + std::to_string(p150) + ")");
    check(p150 > 9300 && p150 < 10000, "150 Hz ~0.970 of input, got " + std::to_string(p150));
    // DC is fully rejected.
    {
        std::array<std::int16_t, kN> dc{};
        dc.fill(3000);
        HighPass hp(75.0, kRate);
        hp.process(dc.data(), dc.size());
        const double mx = settledPeak(dc);
        check(mx < 2.0, "DC rejected, got " + std::to_string(mx));
    }
    // Bypass (cutoff 0) is bit-identical.
    {
        auto x = tone(314.159, 12345.0);
        auto ref = x;
        HighPass hp(0.0, kRate);
        hp.process(x.data(), x.size());
        check(x == ref, "bypass is bit-identical");
    }
    // Full-scale 30 Hz is bounded and settled: steady state ~0.248 * 32767
    // ~ 8128, no growth across the window.
    {
        auto y = tone(30.0, 32767.0);
        HighPass hp(75.0, kRate);
        hp.process(y.data(), y.size());
        const double first = windowPeak(y, kSettle, 2400);
        const double last = windowPeak(y, 3600, kN);
        check(last <= 12000, "full-scale 30 Hz bounded, got " + std::to_string(last));
        check(std::abs(last - first) < 0.02 * first + 1.0,
              "no growth at full-scale 30 Hz (" + std::to_string(first) + " -> " + std::to_string(last) + ")");
    }
    std::cout << "high_pass: ok\n";
    return 0;
}

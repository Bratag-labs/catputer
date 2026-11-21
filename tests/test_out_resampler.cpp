#include "out_resampler.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using lofi::OutputResampler;

// Single-bin Goertzel amplitude (2x peak estimate) of a 48 kHz signal.
float goertzel(const std::int16_t* x, std::size_t n, float freq) {
    const double w = 2.0 * M_PI * freq / 48000.0;
    const double c = 2.0 * std::cos(w);
    double p0 = 0.0, p1 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double p2 = c * p1 - p0 + x[i];
        p0 = p1;
        p1 = p2;
    }
    return static_cast<float>(std::sqrt(p1 * p1 + p0 * p0 - p0 * p1 * c) * 2.0 /
                              static_cast<double>(n));
}

// Renders `blocks` input blocks of a 32 kHz tone through a fresh resampler.
// The first call emits kEmitted (its tail is held until the next call);
// later calls emit kOutBlock.
std::vector<std::int16_t> renderTone(float freq, float amplitude,
                                     std::size_t blocks) {
    OutputResampler resampler;
    std::array<std::int16_t, OutputResampler::kInBlock> in{};
    std::array<std::int16_t, OutputResampler::kOutBlock> out{};
    std::vector<std::int16_t> all;
    all.reserve(blocks * OutputResampler::kOutBlock);
    std::uint32_t phase = 0;
    for (std::size_t b = 0; b < blocks; ++b) {
        for (std::size_t i = 0; i < OutputResampler::kInBlock; ++i) {
            in[i] = static_cast<std::int16_t>(
                amplitude * std::sin(2.0 * M_PI * freq * phase / 32000.0));
            ++phase;
        }
        const std::size_t r = resampler.process(in.data(), out.data());
        assert(r == (b == 0 ? OutputResampler::kEmitted
                            : OutputResampler::kOutBlock));
        all.insert(all.end(), out.begin(), out.begin() + r);
    }
    return all;
}

// Total outputs after `blocks` calls: 756 first, then 768 each.
std::size_t totalOutputs(std::size_t blocks) {
    return OutputResampler::kEmitted +
           (blocks - 1) * OutputResampler::kOutBlock;
}

void testDcGainAndBlockLength() {
    OutputResampler resampler;
    std::array<std::int16_t, OutputResampler::kInBlock> in{};
    in.fill(3000);
    std::array<std::int16_t, OutputResampler::kOutBlock> out{};
    for (std::size_t b = 0; b < 4; ++b) {
        const std::size_t r = resampler.process(in.data(), out.data());
        assert(r == (b == 0 ? OutputResampler::kEmitted
                            : OutputResampler::kOutBlock));
    }
    // Filter fully loaded: unity DC gain on the interior of the block.
    // The per-phase DC gains are 32708/32797/32797 over 32768, a +/-0.1%
    // ripple (<= 8 levels at this amplitude).
    for (std::size_t i = 32; i + 32 < out.size(); ++i) {
        assert(std::abs(static_cast<int>(out[i]) - 3000) <= 8);
    }
}

void testFullScaleDcDoesNotWrap() {
    OutputResampler resampler;
    std::array<std::int16_t, OutputResampler::kInBlock> in{};
    in.fill(32767);
    std::array<std::int16_t, OutputResampler::kOutBlock> out{};
    for (std::size_t b = 0; b < 4; ++b) {
        const std::size_t r = resampler.process(in.data(), out.data());
        assert(r == (b == 0 ? OutputResampler::kEmitted
                            : OutputResampler::kOutBlock));
    }
    // The highest per-phase gain (1.000885) clamps full scale; the
    // lowest (0.998227) lands at ~32710. No wrap-around either way.
    for (std::size_t i = 32; i + 32 < out.size(); ++i) {
        assert(out[i] >= 31000 && out[i] <= 32767);
    }
}

void testPassbandTones() {
    for (const float freq : {440.0f, 1000.0f, 4000.0f, 8000.0f, 12000.0f}) {
        const std::vector<std::int16_t> all = renderTone(freq, 8000.0f, 8);
        const std::size_t n = 4096;
        const std::size_t start = (all.size() - n) / 2;
        const float amplitude = goertzel(all.data() + start, n, freq);
        // Passband is within 0.1 dB to 14 kHz; 5% leaves room for Q15
        // quantization and windowing.
        assert(amplitude > 8000.0f * 0.95f);
        assert(amplitude < 8000.0f * 1.02f);
    }
}

void testImageSuppression() {
    // A 12 kHz input tone creates a 3x-upsample image at 4 kHz after the
    // 2x decimate. The decimation filter must keep it below -45 dB relative.
    const std::vector<std::int16_t> all = renderTone(12000.0f, 8000.0f, 8);
    const std::size_t n = 4096;
    const std::size_t start = (all.size() - n) / 2;
    const float signal = goertzel(all.data() + start, n, 12000.0f);
    const float image = goertzel(all.data() + start, n, 4000.0f);
    assert(signal > 7600.0f);
    assert(image < signal * 0.006f);  // -44 dB
}

void testNoClickAtBlockBoundaries() {
    // 440 Hz at 48 kHz: the legitimate per-sample slope of the tone is at
    // most 2*A*sin(pi*f/fs) ~ 461 counts; a block-boundary click would jump
    // far beyond that. True block boundaries in the emitted stream sit at
    // index 756, then every 768 (the first block emits 756).
    const std::size_t blocks = 16;
    const std::vector<std::int16_t> all = renderTone(440.0f, 8000.0f, blocks);
    assert(all.size() == totalOutputs(blocks));
    const float maxSlope =
        2.0f * 8000.0f * std::sin(M_PI * 440.0f / 48000.0f);
    const float tolerance = maxSlope + 96.0f;  // slope + ripple + quantization
    std::size_t boundary = 0, interior = 0;
    float boundaryMax = 0.0f, interiorMax = 0.0f;
    for (std::size_t i = 1; i < all.size(); ++i) {
        const float delta = std::abs(
            static_cast<float>(all[i]) - static_cast<float>(all[i - 1]));
        const bool atBoundary = (i >= OutputResampler::kEmitted) &&
                                ((i - OutputResampler::kEmitted) %
                                 OutputResampler::kOutBlock == 0);
        if (atBoundary) {
            ++boundary;
            boundaryMax = std::max(boundaryMax, delta);
        } else {
            ++interior;
            interiorMax = std::max(interiorMax, delta);
        }
    }
    assert(boundary == blocks - 1);
    assert(interior > 0);
    assert(boundaryMax <= tolerance);
    assert(interiorMax <= tolerance);
}

void testMonotoneRampHasNoAliasingRipple() {
    // A 16 ms 32 kHz ramp (62.5 Hz) is well below both Nyquist limits; the
    // output must stay monotone apart from the per-phase DC ripple: the
    // phase gains 0.998227/1.000885/1.000885 differ by 0.27%, i.e. at most
    // ~80 levels on a full-scale signal.
    OutputResampler resampler;
    std::array<std::int16_t, OutputResampler::kInBlock> in{};
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = static_cast<std::int16_t>(30000.0 * i / static_cast<float>(in.size()));
    }
    std::array<std::int16_t, OutputResampler::kOutBlock> out{};
    for (int b = 0; b < 4; ++b) {
        resampler.process(in.data(), out.data());
        // The ramp restarts at the input block edge, which lands mid-output
        // block (3/2 ratio); check monotonicity within the interior of each
        // output block away from the wrap.
        for (std::size_t i = 48; i + 48 < out.size(); ++i) {
            assert(out[i + 1] >= out[i] - 96);
        }
    }
}

}  // namespace

int main() {
    testDcGainAndBlockLength();
    testFullScaleDcDoesNotWrap();
    testPassbandTones();
    testImageSuppression();
    testNoClickAtBlockBoundaries();
    testMonotoneRampHasNoAliasingRipple();
    return 0;
}

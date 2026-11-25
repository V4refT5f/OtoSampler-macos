// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cmath>
#include <complex>
#include <vector>
namespace oto
{
struct FilterSettings
{
    bool  on = false;
    float hp = 20.f;      // high-pass cutoff (Hz), 20 = off
    float lp = 20000.f;   // low-pass cutoff (Hz), 20000 = off
    bool operator== (const FilterSettings& o) const noexcept { return on == o.on && hp == o.hp && lp == o.lp; }
};

// RBJ biquad (transposed direct form II, double precision)
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set (int type, double fs, double fc, double q)
    {
        fc = std::fmin (std::fmax (fc, 10.0), fs * 0.45);
        q = std::fmax (q, 0.1);
        const double w = 6.283185307179586 * fc / fs, cw = std::cos (w), sw = std::sin (w), al = sw / (2.0 * q);
        double a0 = 1.0 + al;
        if (type == 0)      { b0 = (1 - cw) * 0.5; b1 = 1 - cw; b2 = b0; }
        else if (type == 1) { b0 = (1 + cw) * 0.5; b1 = -(1 + cw); b2 = b0; }
        else                { b0 = al; b1 = 0; b2 = -al; }
        a1 = -2.0 * cw; a2 = 1.0 - al;
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }
    // Frequency response at angular frequency w (radians per sample)
    std::complex<double> response (double w) const noexcept
    {
        const std::complex<double> z1i = std::polar (1.0, -w), z2i = std::polar (1.0, -2.0 * w);
        return (b0 + b1 * z1i + b2 * z2i) / (1.0 + a1 * z1i + a2 * z2i);
    }
    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// 16th-order Butterworth (8 cascaded biquads) = 96 dB/oct
struct ButterworthCascade
{
    static constexpr int kSections = 8;
    Biquad s[kSections];
    void set (int type, double fs, double fc)
    {
        for (int k = 1; k <= kSections; ++k)
        {
            const double q = 1.0 / (2.0 * std::sin ((2.0 * k - 1.0) * 3.14159265358979323846 / (4.0 * kSections)));
            s[k - 1].set (type, fs, fc, q);
        }
    }
    std::complex<double> response (double w) const noexcept
    {
        std::complex<double> h (1.0, 0.0);
        for (const auto& b : s) h *= b.response (w);
        return h;
    }
    double process (double x) noexcept
    {
        for (auto& b : s) x = b.process (x);
        return x;
    }
};

// HP and LP can be active at the same time, 96 dB/oct each,
// applied offline to the whole sample (before pitch / formant processing).
inline void applyPreFilter (std::vector<float>& data, double fs, const FilterSettings& s)
{
    if (! s.on) return;
    const bool useHp = s.hp > 20.5f;
    const bool useLp = s.lp < 19990.f && s.lp < (float) (fs * 0.45);
    if (! useHp && ! useLp) return;
    ButterworthCascade hp, lp;
    hp.set (1, fs, s.hp);
    lp.set (0, fs, s.lp);
    for (auto& v : data)
    {
        double x = (double) v;
        if (useHp) x = hp.process (x);
        if (useLp) x = lp.process (x);
        v = (float) x;
    }
}
}

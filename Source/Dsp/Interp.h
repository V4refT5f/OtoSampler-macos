#pragma once
#include <algorithm>
#include <cmath>

namespace oto
{
constexpr double kPi = 3.14159265358979323846;

struct HannTable
{
    float t[1026];
    HannTable() { for (int i = 0; i < 1026; ++i) t[i] = 0.5f - 0.5f * (float) std::cos (2.0 * kPi * i / 1024.0); }
};
inline const HannTable& hannTable() { static const HannTable h; return h; }

// periodic Hann, x in 0..1
inline float hannAt (float x) noexcept
{
    const auto& t = hannTable().t;
    const float p = std::clamp (x, 0.f, 1.f) * 1024.f; const int i = (int) p; const float f = p - (float) i;
    return t[i] + (t[i + 1] - t[i]) * f;
}

inline float hermite (float y0, float y1, float y2, float y3, float f) noexcept
{
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

// 4-point Hermite, zero outside the buffer
inline float readSample (const float* d, int n, double pos) noexcept
{
    const int i = (int) std::floor (pos);
    const float f = (float) (pos - i);
    float y0, y1, y2, y3;
    if (i >= 1 && i + 2 < n) { y0 = d[i - 1]; y1 = d[i]; y2 = d[i + 1]; y3 = d[i + 2]; }
    else
    {
        auto at = [&] (int k) { return (k >= 0 && k < n) ? d[k] : 0.f; };
        y0 = at (i - 1); y1 = at (i); y2 = at (i + 1); y3 = at (i + 2);
    }
    return hermite (y0, y1, y2, y3, f);
}

inline float crossW2 (double m) noexcept   // 0..1 -> sin^2 (sums to 1 with cos^2)
{
    const double s = std::sin (std::clamp (m, 0.0, 1.0) * kPi * 0.5);
    return (float) (s * s);
}
}

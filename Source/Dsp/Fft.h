// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <complex>
#include <vector>

namespace oto
{
// Small radix-2 FFT (forward only, double precision). Size must be a power of two.
class Fft
{
public:
    explicit Fft (int n) : N (n), rev ((size_t) n), tw ((size_t) (n / 2 > 0 ? n / 2 : 1))
    {
        int lg = 0;
        while ((1 << lg) < n) ++lg;
        for (int i = 0; i < n; ++i)
            rev[(size_t) i] = lg == 0 ? 0 : (rev[(size_t) (i >> 1)] >> 1) | ((i & 1) << (lg - 1));
        for (int k = 0; k < n / 2; ++k)
            tw[(size_t) k] = std::polar (1.0, -6.283185307179586 * (double) k / (double) n);
    }

    int size() const noexcept { return N; }

    void forward (std::complex<double>* x) const noexcept
    {
        for (int i = 0; i < N; ++i)
        {
            const int j = rev[(size_t) i];
            if (i < j) std::swap (x[i], x[j]);
        }
        for (int len = 2; len <= N; len <<= 1)
        {
            const int half = len >> 1, step = N / len;
            for (int i = 0; i < N; i += len)
                for (int j = 0; j < half; ++j)
                {
                    const std::complex<double> w = tw[(size_t) (j * step)];
                    const std::complex<double> u = x[i + j], v = x[i + j + half] * w;
                    x[i + j] = u + v;
                    x[i + j + half] = u - v;
                }
        }
    }

private:
    int N;
    std::vector<int> rev;
    std::vector<std::complex<double>> tw;
};
}

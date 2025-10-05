#include "matrix_convolver/fft_engine.h"

#include <cmath>
#include <cstddef>

namespace spcmic {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

bool FftEngine::isPowerOfTwo(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

void FftEngine::forward(std::vector<std::complex<float>>& data) {
    transform(data, false);
}

void FftEngine::inverse(std::vector<std::complex<float>>& data) {
    transform(data, true);
}

void FftEngine::transform(std::vector<std::complex<float>>& data, bool inverse) {
    const size_t n = data.size();
    if (!isPowerOfTwo(n) || n < 2) {
        return;
    }

    // Bit-reversal permutation
    size_t j = 0;
    for (size_t i = 1; i < n; ++i) {
        size_t bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;

        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    // Iterative Cooley-Tukey
    for (size_t len = 2; len <= n; len <<= 1) {
        const float angle = 2.0f * kPi / static_cast<float>(len) * (inverse ? -1.0f : 1.0f);
        const std::complex<float> wLen(cosf(angle), sinf(angle));

        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            const size_t halfLen = len >> 1;

            for (size_t k = 0; k < halfLen; ++k) {
                const std::complex<float> u = data[i + k];
                const std::complex<float> v = data[i + k + halfLen] * w;
                data[i + k] = u + v;
                data[i + k + halfLen] = u - v;
                w *= wLen;
            }
        }
    }

    if (inverse) {
        const float invN = 1.0f / static_cast<float>(n);
        for (auto& value : data) {
            value *= invN;
        }
    }
}

} // namespace spcmic

#include "matrix_convolver/fft_engine.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace spcmic {

namespace {
constexpr float kPi = 3.14159265358979323846f;

struct StagePlan {
    size_t length;
    std::vector<std::complex<float>> twiddles; // Forward twiddles for this stage
};

struct FftPlan {
    size_t size;
    std::vector<int> bitReverse;
    std::vector<StagePlan> stages;
};

FftPlan buildPlan(size_t n) {
    FftPlan plan{};
    plan.size = n;
    plan.bitReverse.resize(n);

    unsigned int bits = 0;
    size_t temp = n;
    while (temp > 1) {
        ++bits;
        temp >>= 1;
    }
    for (size_t i = 0; i < n; ++i) {
        unsigned int reversed = 0;
        unsigned int value = static_cast<unsigned int>(i);
        for (unsigned int b = 0; b < bits; ++b) {
            reversed = (reversed << 1u) | (value & 1u);
            value >>= 1u;
        }
        plan.bitReverse[i] = static_cast<int>(reversed);
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        StagePlan stage{};
        stage.length = len;
        const size_t halfLen = len >> 1;
        stage.twiddles.resize(halfLen);
        const float baseAngle = -2.0f * kPi / static_cast<float>(len);
        for (size_t k = 0; k < halfLen; ++k) {
            const float angle = baseAngle * static_cast<float>(k);
            stage.twiddles[k] = std::complex<float>(std::cos(angle), std::sin(angle));
        }
        plan.stages.emplace_back(std::move(stage));
    }

    return plan;
}

FftPlan& getPlan(size_t n) {
    static std::mutex mutex;
    static std::unordered_map<size_t, FftPlan> plans;
    std::lock_guard<std::mutex> lock(mutex);

    auto it = plans.find(n);
    if (it != plans.end()) {
        return it->second;
    }

    auto [insertIt, _] = plans.emplace(n, buildPlan(n));
    return insertIt->second;
}
} // namespace

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

    auto& plan = getPlan(n);

    thread_local std::unordered_map<size_t, std::vector<std::complex<float>>> localBuffers;
    auto& buffer = localBuffers[n];
    if (buffer.size() != n) {
        buffer.assign(n, std::complex<float>(0.0f, 0.0f));
    }

    for (size_t i = 0; i < n; ++i) {
        buffer[i] = data[plan.bitReverse[i]];
    }

    for (const auto& stage : plan.stages) {
        const size_t len = stage.length;
        const size_t halfLen = len >> 1;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < halfLen; ++k) {
                const auto& w = inverse ? std::conj(stage.twiddles[k]) : stage.twiddles[k];
                const std::complex<float> u = buffer[i + k];
                const std::complex<float> v = buffer[i + k + halfLen] * w;
                buffer[i + k] = u + v;
                buffer[i + k + halfLen] = u - v;
            }
        }
    }

    if (inverse) {
        const float invN = 1.0f / static_cast<float>(n);
        for (size_t i = 0; i < n; ++i) {
            data[i] = buffer[i] * invN;
        }
    } else {
        std::memcpy(data.data(), buffer.data(), n * sizeof(std::complex<float>));
    }
}

} // namespace spcmic

#include "matrix_convolver/fft_engine.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <android/log.h>

namespace spcmic {

namespace {
constexpr float kPi = 3.14159265358979323846f;

using Clock = std::chrono::steady_clock;

struct TimingState {
    long long totalMicros = 0;
    int calls = 0;
};

std::mutex& timingMutex() {
    static std::mutex mutex;
    return mutex;
}

TimingState& forwardTiming() {
    static TimingState state;
    return state;
}

TimingState& inverseTiming() {
    static TimingState state;
    return state;
}

void recordTiming(TimingState& state, long long micros, const char* label) {
    if (micros <= 0) {
        return;
    }

    auto& mutex = timingMutex();
    std::lock_guard<std::mutex> lock(mutex);
    state.totalMicros += micros;
    state.calls += 1;
    constexpr int kLogInterval = 512;
    if (state.calls >= kLogInterval) {
        const double avgMs = static_cast<double>(state.totalMicros) / static_cast<double>(state.calls) / 1000.0;
        __android_log_print(ANDROID_LOG_DEBUG, "FftEngine", "%s avg %.3f ms over %d calls", label, avgMs, state.calls);
        state.totalMicros = 0;
        state.calls = 0;
    }
}

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
    const auto start = Clock::now();
    transform(data, false);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    recordTiming(forwardTiming(), elapsed, "FFT forward");
}

void FftEngine::inverse(std::vector<std::complex<float>>& data) {
    const auto start = Clock::now();
    transform(data, true);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    recordTiming(inverseTiming(), elapsed, "FFT inverse");
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

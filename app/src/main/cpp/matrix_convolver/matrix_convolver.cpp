#include "matrix_convolver/matrix_convolver.h"
#include "matrix_convolver/fft_engine.h"

#include <algorithm>
#include <android/log.h>
#include <chrono>
#include <mutex>
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#define LOG_TAG "MatrixConvolver"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
constexpr int kNumChannels = 84;
constexpr std::complex<float> kZeroComplex{0.0f, 0.0f};
#if defined(SPCMIC_ENABLE_ACCUM_TIMING)
using Clock = std::chrono::steady_clock;

struct AccumTimingState {
    long long totalMicros = 0;
    int blocks = 0;
};

std::mutex& accumulationMutex() {
    static std::mutex mutex;
    return mutex;
}

AccumTimingState& accumulationState() {
    static AccumTimingState state;
    return state;
}

void recordAccumulation(long long micros) {
    if (micros <= 0) {
        return;
    }

    auto& state = accumulationState();
    auto& mutex = accumulationMutex();
    std::lock_guard<std::mutex> lock(mutex);
    state.totalMicros += micros;
    state.blocks += 1;
    constexpr int kLogInterval = 32;
    if (state.blocks >= kLogInterval) {
        const double avgMs = static_cast<double>(state.totalMicros) / static_cast<double>(state.blocks) / 1000.0;
        LOGD("Accumulation avg %.3f ms over %d blocks", avgMs, state.blocks);
        state.totalMicros = 0;
        state.blocks = 0;
    }
}
#else
inline void recordAccumulation(long long) {}
#endif

#if defined(__clang__)
#define SPCMIC_VECTORIZE _Pragma("clang loop vectorize(enable)")
#else
#define SPCMIC_VECTORIZE
#endif

inline void accumulatePartition(const std::vector<std::complex<float>>& inputSpectrum,
                                const std::vector<std::complex<float>>& irSpectrum,
                                std::vector<std::complex<float>>& accumulator) {
    const int bins = static_cast<int>(accumulator.size());
    const float* input = reinterpret_cast<const float*>(inputSpectrum.data());
    const float* ir = reinterpret_cast<const float*>(irSpectrum.data());
    float* acc = reinterpret_cast<float*>(accumulator.data());

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
    int bin = 0;
    const int floatStride = 2; // real + imag per complex sample
    for (; bin + 3 < bins; bin += 4) {
        const float* inputPtr = input + bin * floatStride;
        const float* irPtr = ir + bin * floatStride;
        float* accPtr = acc + bin * floatStride;

        float32x4x2_t inputVec = vld2q_f32(inputPtr);
        float32x4x2_t irVec = vld2q_f32(irPtr);
        float32x4x2_t accVec = vld2q_f32(accPtr);

        float32x4_t rr = vmulq_f32(inputVec.val[0], irVec.val[0]);
        float32x4_t ii = vmulq_f32(inputVec.val[1], irVec.val[1]);
        float32x4_t ri = vmulq_f32(inputVec.val[0], irVec.val[1]);
        float32x4_t irPart = vmulq_f32(inputVec.val[1], irVec.val[0]);

        float32x4_t real = vsubq_f32(rr, ii);
        float32x4_t imag = vaddq_f32(ri, irPart);

        accVec.val[0] = vaddq_f32(accVec.val[0], real);
        accVec.val[1] = vaddq_f32(accVec.val[1], imag);

        vst2q_f32(accPtr, accVec);
    }

    for (; bin < bins; ++bin) {
        const int offset = bin * floatStride;
        const float aRe = input[offset];
        const float aIm = input[offset + 1];
        const float bRe = ir[offset];
        const float bIm = ir[offset + 1];
        const float real = aRe * bRe - aIm * bIm;
        const float imag = aRe * bIm + aIm * bRe;
        acc[offset] += real;
        acc[offset + 1] += imag;
    }
#else
    SPCMIC_VECTORIZE
    for (int bin = 0; bin < bins; ++bin) {
        accumulator[bin] += inputSpectrum[bin] * irSpectrum[bin];
    }
#endif
}
}

namespace spcmic {

MatrixConvolver::MatrixConvolver()
    : impulseResponse_(nullptr)
    , blockSize_(0)
    , ready_(false)
    , fftSize_(0)
    , numPartitions_(0)
    , numOutputChannels_(0)
    , historyWritePos_(0)
    , outputGain_(1.0f)
    , singlePartition_(false) {
}

bool MatrixConvolver::configure(const MatrixImpulseResponse* ir, int blockSizeFrames) {
    if (!ir || !ir->isValid() || blockSizeFrames <= 0) {
        impulseResponse_ = nullptr;
        blockSize_ = 0;
        ready_ = false;
        channelStates_.clear();
        channelIRs_.clear();
        freqAccum_.clear();
        overlap_.clear();
        fftSize_ = 0;
        numPartitions_ = 0;
        numOutputChannels_ = 0;
        historyWritePos_ = 0;
        return false;
    }

    impulseResponse_ = ir;
    blockSize_ = blockSizeFrames;
    numOutputChannels_ = impulseResponse_->numOutputChannels;

    if (numOutputChannels_ <= 0) {
        LOGE("Impulse response reports zero output channels");
        ready_ = false;
        return false;
    }

    if (!FftEngine::isPowerOfTwo(static_cast<size_t>(blockSize_))) {
        LOGE("MatrixConvolver requires power-of-two block size. Got %d", blockSize_);
        ready_ = false;
        return false;
    }

    fftSize_ = blockSize_ * 2;
    numPartitions_ = (impulseResponse_->irLength + blockSize_ - 1) / blockSize_;

    if (numPartitions_ <= 0) {
        LOGE("Invalid partition count");
        ready_ = false;
        return false;
    }

    singlePartition_ = (numPartitions_ == 1);

    channelIRs_.assign(impulseResponse_->numInputChannels, ChannelIR{});
    channelStates_.assign(impulseResponse_->numInputChannels, ChannelState{});

    for (auto& state : channelStates_) {
        state.history.assign(numPartitions_, std::vector<std::complex<float>>(fftSize_, kZeroComplex));
    }

    for (int ch = 0; ch < impulseResponse_->numInputChannels; ++ch) {
        ChannelIR& channelIR = channelIRs_[ch];
        channelIR.partitions.assign(static_cast<size_t>(numOutputChannels_) * numPartitions_,
                                    std::vector<std::complex<float>>(fftSize_, kZeroComplex));

        for (int outCh = 0; outCh < numOutputChannels_; ++outCh) {
            const size_t impulseOffset = (static_cast<size_t>(outCh) * impulseResponse_->numInputChannels +
                                          static_cast<size_t>(ch)) * static_cast<size_t>(impulseResponse_->irLength);
            for (int p = 0; p < numPartitions_; ++p) {
                auto& partition = channelIR.partitions[static_cast<size_t>(outCh) * numPartitions_ + p];
                for (int n = 0; n < blockSize_; ++n) {
                    const size_t irIndex = impulseOffset + static_cast<size_t>(p) * blockSize_ + static_cast<size_t>(n);
                    if (irIndex < impulseOffset + static_cast<size_t>(impulseResponse_->irLength)) {
                        const float sample = impulseResponse_->impulseData[irIndex];
                        partition[n] = std::complex<float>(sample, 0.0f);
                    }
                }
                FftEngine::forward(partition);
            }
        }
    }

    freqAccum_.assign(numOutputChannels_, std::vector<std::complex<float>>(fftSize_, kZeroComplex));
    overlap_.assign(numOutputChannels_, std::vector<float>(blockSize_, 0.0f));
    historyWritePos_ = 0;

    ready_ = true;

    LOGD("MatrixConvolver configured: sampleRate=%d, irLength=%d, partitions=%d, fftSize=%d",
         impulseResponse_->sampleRate,
         impulseResponse_->irLength,
         numPartitions_,
         fftSize_);

    reset();
    return ready_;
}

void MatrixConvolver::reset() {
    for (auto& overlapChannel : overlap_) {
        std::fill(overlapChannel.begin(), overlapChannel.end(), 0.0f);
    }
    historyWritePos_ = 0;

    for (auto& state : channelStates_) {
        for (auto& block : state.history) {
            std::fill(block.begin(), block.end(), kZeroComplex);
        }
    }
}

void MatrixConvolver::process(const float* input, float* output, int numFrames) {
    if (!ready_ || !impulseResponse_ || !input || !output || numFrames != blockSize_) {
        static bool loggedFallback = false;
        if (!loggedFallback) {
            LOGW("MatrixConvolver fallback engaged (ready=%d, ir=%p, frames=%d, block=%d)",
                 ready_, impulseResponse_, numFrames, blockSize_);
            loggedFallback = true;
        }
        fallbackDownmix(input, output, numFrames);
        return;
    }

    for (auto& accum : freqAccum_) {
        std::fill(accum.begin(), accum.end(), kZeroComplex);
    }

    const int numChannels = impulseResponse_->numInputChannels;

    #if defined(SPCMIC_ENABLE_ACCUM_TIMING)
    long long accumulateMicros = 0;
    #endif

    if (singlePartition_) {
        for (int ch = 0; ch < numChannels; ++ch) {
            auto& spectrum = channelStates_[ch].history[0];

            for (int frame = 0; frame < blockSize_; ++frame) {
                spectrum[frame] = std::complex<float>(input[frame * numChannels + ch], 0.0f);
            }
            std::fill(spectrum.begin() + blockSize_, spectrum.end(), kZeroComplex);

            FftEngine::forward(spectrum);

            #if defined(SPCMIC_ENABLE_ACCUM_TIMING)
            const auto accumStart = Clock::now();
            #endif
            for (int outCh = 0; outCh < numOutputChannels_; ++outCh) {
                const auto& irPartition = channelIRs_[ch].partitions[static_cast<size_t>(outCh) * numPartitions_];
                accumulatePartition(spectrum, irPartition, freqAccum_[outCh]);
            }
            #if defined(SPCMIC_ENABLE_ACCUM_TIMING)
            accumulateMicros += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - accumStart).count();
            #endif
        }
    } else {
        for (int ch = 0; ch < numChannels; ++ch) {
            auto& historyBlocks = channelStates_[ch].history;
            auto& spectrum = historyBlocks[historyWritePos_];

            for (int frame = 0; frame < blockSize_; ++frame) {
                spectrum[frame] = std::complex<float>(input[frame * numChannels + ch], 0.0f);
            }
            std::fill(spectrum.begin() + blockSize_, spectrum.end(), kZeroComplex);

            FftEngine::forward(spectrum);

            const ChannelIR& channelIR = channelIRs_[ch];

            #if defined(SPCMIC_ENABLE_ACCUM_TIMING)
            const auto accumStart = Clock::now();
            #endif
            for (int p = 0; p < numPartitions_; ++p) {
                const int histIndex = (historyWritePos_ - p + numPartitions_) % numPartitions_;
                const auto& inputSpectrum = historyBlocks[histIndex];
                for (int outCh = 0; outCh < numOutputChannels_; ++outCh) {
                    const auto& irSpectrum = channelIR.partitions[static_cast<size_t>(outCh) * numPartitions_ + p];
                    accumulatePartition(inputSpectrum, irSpectrum, freqAccum_[outCh]);
                }
            }
            #if defined(SPCMIC_ENABLE_ACCUM_TIMING)
            accumulateMicros += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - accumStart).count();
            #endif
        }

        historyWritePos_ = (historyWritePos_ + 1) % numPartitions_;
    }

    // Inverse FFT to obtain time-domain output
    for (int outCh = 0; outCh < numOutputChannels_; ++outCh) {
        FftEngine::inverse(freqAccum_[outCh]);
    }

    for (int frame = 0; frame < blockSize_; ++frame) {
        for (int outCh = 0; outCh < numOutputChannels_; ++outCh) {
            const float value = (freqAccum_[outCh][frame].real() + overlap_[outCh][frame]) * outputGain_;
            output[frame * numOutputChannels_ + outCh] = value;
        }
    }

    for (int frame = 0; frame < blockSize_; ++frame) {
        for (int outCh = 0; outCh < numOutputChannels_; ++outCh) {
            overlap_[outCh][frame] = freqAccum_[outCh][frame + blockSize_].real();
        }
    }

    #if defined(SPCMIC_ENABLE_ACCUM_TIMING)
    recordAccumulation(accumulateMicros);
    #endif
}

void MatrixConvolver::fallbackDownmix(const float* input, float* output, int numFrames) const {
    if (!input || !output || numFrames <= 0) {
        return;
    }

    const int numChannels = impulseResponse_ ? impulseResponse_->numInputChannels : kNumChannels;
    const int outputs = numOutputChannels_ > 0 ? numOutputChannels_ : 2;

    for (int frame = 0; frame < numFrames; ++frame) {
        const float sample = input[frame * numChannels];
        for (int outCh = 0; outCh < outputs; ++outCh) {
            output[frame * outputs + outCh] = sample;
        }
    }
}

} // namespace spcmic

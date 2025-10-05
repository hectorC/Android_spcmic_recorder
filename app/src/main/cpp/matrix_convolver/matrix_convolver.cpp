#include "matrix_convolver/matrix_convolver.h"
#include "matrix_convolver/fft_engine.h"

#include <algorithm>
#include <android/log.h>

#define LOG_TAG "MatrixConvolver"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
constexpr int kNumChannels = 84;
constexpr std::complex<float> kZeroComplex{0.0f, 0.0f};

#if defined(__clang__)
#define SPCMIC_VECTORIZE _Pragma("clang loop vectorize(enable)")
#else
#define SPCMIC_VECTORIZE
#endif

inline void accumulatePartition(const std::vector<std::complex<float>>& inputSpectrum,
                                const std::vector<std::complex<float>>& irSpectrum,
                                std::vector<std::complex<float>>& accumulator) {
    const int bins = static_cast<int>(accumulator.size());
    const auto* input = inputSpectrum.data();
    const auto* ir = irSpectrum.data();
    auto* acc = accumulator.data();
    SPCMIC_VECTORIZE
    for (int bin = 0; bin < bins; ++bin) {
        acc[bin] += input[bin] * ir[bin];
    }
}
}

namespace spcmic {

MatrixConvolver::MatrixConvolver()
    : impulseResponse_(nullptr)
    , blockSize_(0)
    , ready_(false)
    , fftSize_(0)
    , numPartitions_(0)
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
    freqAccumLeft_.clear();
    freqAccumRight_.clear();
        overlapLeft_.clear();
        overlapRight_.clear();
        fftSize_ = 0;
        numPartitions_ = 0;
        historyWritePos_ = 0;
        return false;
    }

    impulseResponse_ = ir;
    blockSize_ = blockSizeFrames;

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
        channelIR.partitionsLeft.assign(numPartitions_, std::vector<std::complex<float>>(fftSize_, kZeroComplex));
        channelIR.partitionsRight.assign(numPartitions_, std::vector<std::complex<float>>(fftSize_, kZeroComplex));

        for (int p = 0; p < numPartitions_; ++p) {
            auto& leftPart = channelIR.partitionsLeft[p];
            auto& rightPart = channelIR.partitionsRight[p];

            const int baseIndex = ch * impulseResponse_->irLength + p * blockSize_;
            for (int n = 0; n < blockSize_; ++n) {
                const int irIndex = baseIndex + n;
                if (irIndex < (ch + 1) * impulseResponse_->irLength) {
                    const float leftSample = impulseResponse_->leftEar[irIndex];
                    const float rightSample = impulseResponse_->rightEar[irIndex];
                    leftPart[n] = std::complex<float>(leftSample, 0.0f);
                    rightPart[n] = std::complex<float>(rightSample, 0.0f);
                }
            }

            FftEngine::forward(leftPart);
            FftEngine::forward(rightPart);
        }
    }

    freqAccumLeft_.assign(fftSize_, kZeroComplex);
    freqAccumRight_.assign(fftSize_, kZeroComplex);
    overlapLeft_.assign(blockSize_, 0.0f);
    overlapRight_.assign(blockSize_, 0.0f);
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
    std::fill(overlapLeft_.begin(), overlapLeft_.end(), 0.0f);
    std::fill(overlapRight_.begin(), overlapRight_.end(), 0.0f);
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

    std::fill(freqAccumLeft_.begin(), freqAccumLeft_.end(), kZeroComplex);
    std::fill(freqAccumRight_.begin(), freqAccumRight_.end(), kZeroComplex);

    const int numChannels = impulseResponse_->numInputChannels;

    if (singlePartition_) {
        for (int ch = 0; ch < numChannels; ++ch) {
            auto& spectrum = channelStates_[ch].history[0];

            for (int frame = 0; frame < blockSize_; ++frame) {
                spectrum[frame] = std::complex<float>(input[frame * numChannels + ch], 0.0f);
            }
            std::fill(spectrum.begin() + blockSize_, spectrum.end(), kZeroComplex);

            FftEngine::forward(spectrum);

            const auto& leftIR = channelIRs_[ch].partitionsLeft[0];
            const auto& rightIR = channelIRs_[ch].partitionsRight[0];

            accumulatePartition(spectrum, leftIR, freqAccumLeft_);
            accumulatePartition(spectrum, rightIR, freqAccumRight_);
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

            for (int p = 0; p < numPartitions_; ++p) {
                const int histIndex = (historyWritePos_ - p + numPartitions_) % numPartitions_;
                const auto& inputSpectrum = historyBlocks[histIndex];
                const auto& leftIR = channelIR.partitionsLeft[p];
                const auto& rightIR = channelIR.partitionsRight[p];

                accumulatePartition(inputSpectrum, leftIR, freqAccumLeft_);
                accumulatePartition(inputSpectrum, rightIR, freqAccumRight_);
            }
        }

        historyWritePos_ = (historyWritePos_ + 1) % numPartitions_;
    }

    // Inverse FFT to obtain time-domain output
    FftEngine::inverse(freqAccumLeft_);
    FftEngine::inverse(freqAccumRight_);

    for (int frame = 0; frame < blockSize_; ++frame) {
    const float left = (freqAccumLeft_[frame].real() + overlapLeft_[frame]) * outputGain_;
    const float right = (freqAccumRight_[frame].real() + overlapRight_[frame]) * outputGain_;
    output[frame * 2] = left;
    output[frame * 2 + 1] = right;
    }

    for (int frame = 0; frame < blockSize_; ++frame) {
        overlapLeft_[frame] = freqAccumLeft_[frame + blockSize_].real();
        overlapRight_[frame] = freqAccumRight_[frame + blockSize_].real();
    }
}

void MatrixConvolver::fallbackDownmix(const float* input, float* output, int numFrames) const {
    if (!input || !output || numFrames <= 0) {
        return;
    }

    const int numChannels = impulseResponse_ ? impulseResponse_->numInputChannels : kNumChannels;

    for (int frame = 0; frame < numFrames; ++frame) {
        const float ch0 = input[frame * numChannels];
        output[frame * 2] = ch0;
        output[frame * 2 + 1] = ch0;
    }
}

} // namespace spcmic

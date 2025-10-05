#ifndef SPCMIC_MATRIX_CONVOLVER_H
#define SPCMIC_MATRIX_CONVOLVER_H

#include <cstdint>
#include <vector>
#include <complex>
#include "matrix_convolver/ir_data.h"

namespace spcmic {

class MatrixConvolver {
public:
    MatrixConvolver();

    /**
     * Configure the convolver with the impulse response data and block size.
     * The impulse response object must outlive the convolver.
     */
    bool configure(const MatrixImpulseResponse* ir, int blockSizeFrames);

    void reset();

    [[nodiscard]] bool isReady() const { return ready_; }

    void setOutputGain(float gain) { outputGain_ = gain; }

    /**
     * Process a block of multichannel input.
     * @param input Interleaved float array: numFrames * numChannels samples.
     * @param output Interleaved stereo float array: numFrames * 2 samples.
     * @param numFrames Number of frames in this block.
     */
    void process(const float* input, float* output, int numFrames);

private:
    struct ChannelState {
        std::vector<std::vector<std::complex<float>>> history; // [partition][fftBin]
    };

    struct ChannelIR {
        std::vector<std::vector<std::complex<float>>> partitionsLeft;
        std::vector<std::vector<std::complex<float>>> partitionsRight;
    };

    void fallbackDownmix(const float* input, float* output, int numFrames) const;

    const MatrixImpulseResponse* impulseResponse_;
    int blockSize_;
    bool ready_;

    int fftSize_;
    int numPartitions_;
    int historyWritePos_;

    std::vector<ChannelState> channelStates_;
    std::vector<ChannelIR> channelIRs_;

    std::vector<std::complex<float>> freqAccumLeft_;
    std::vector<std::complex<float>> freqAccumRight_;
    std::vector<float> overlapLeft_;
    std::vector<float> overlapRight_;

    float outputGain_;
    bool singlePartition_;
};

} // namespace spcmic

#endif // SPCMIC_MATRIX_CONVOLVER_H

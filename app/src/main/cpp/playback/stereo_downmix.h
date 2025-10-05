#ifndef SPCMIC_STEREO_DOWNMIX_H
#define SPCMIC_STEREO_DOWNMIX_H

#include <cstdint>

namespace spcmic {

/**
 * Simple stereo downmix from 84 channels
 * Sums all channels with scaling to prevent clipping
 */
class StereoDownmix {
public:
    StereoDownmix();
    ~StereoDownmix() = default;

    /**
     * Process 84-channel interleaved input to stereo output
     * @param input Interleaved 84-channel audio (numFrames * 84 samples)
     * @param output Stereo output buffer (numFrames * 2 samples)
     * @param numFrames Number of frames to process
     */
    void process(const float* input, float* output, int32_t numFrames);

    /**
     * Set gain scaling factor
     * @param gain Linear gain (default: 1.0 / sqrt(84) ≈ 0.109)
     */
    void setGain(float gain);

private:
    static constexpr int32_t NUM_INPUT_CHANNELS = 84;
    static constexpr int32_t NUM_OUTPUT_CHANNELS = 2;
    
    float gain_;
    float peakLevel_;  // Track peak for diagnostics
};

} // namespace spcmic

#endif // SPCMIC_STEREO_DOWNMIX_H

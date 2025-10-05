#include "stereo_downmix.h"
#include <cmath>
#include <cstring>
#include <android/log.h>

#define LOG_TAG "StereoDownmix"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace spcmic {

StereoDownmix::StereoDownmix()
    : gain_(1.0f / 84.0f)  // Conservative peak scaling (was 1/√84 = 0.109, now 1/84 = 0.012)
    , peakLevel_(0.0f) {
    LOGD("StereoDownmix initialized with gain=%.6f", gain_);
}

void StereoDownmix::setGain(float gain) {
    gain_ = gain;
}

void StereoDownmix::process(const float* input, float* output, int32_t numFrames) {
    // TEMPORARY: Just play channel 0 to both L+R (bypass summation to test playback)
    // This will help us verify the playback engine and WAV reader are working correctly
    
    float maxSample = 0.0f;
    
    for (int32_t frame = 0; frame < numFrames; frame++) {
        // Get first channel (channel 0) from interleaved 84-channel input
        float ch0_sample = input[frame * NUM_INPUT_CHANNELS + 0];
        
        // Copy to both left and right (no gain needed for single channel)
        output[frame * 2 + 0] = ch0_sample;  // Left
        output[frame * 2 + 1] = ch0_sample;  // Right
        
        // Track peak for diagnostics
        float absSample = std::abs(ch0_sample);
        if (absSample > maxSample) {
            maxSample = absSample;
        }
    }
    
    // Log peak level
    if (maxSample > peakLevel_) {
        peakLevel_ = maxSample;
        if (maxSample > 1.0f) {
            LOGW("CLIPPING in channel 0! Peak level: %.2f", maxSample);
        } else if (maxSample > 0.01f) {
            LOGD("Channel 0 peak level: %.4f", maxSample);
        }
    }
}

} // namespace spcmic

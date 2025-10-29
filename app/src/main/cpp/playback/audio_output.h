#ifndef SPCMIC_AUDIO_OUTPUT_H
#define SPCMIC_AUDIO_OUTPUT_H

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <cstdint>
#include <functional>

namespace spcmic {

/**
 * Low-latency audio output using OpenSL ES
 * Handles stereo PCM float output
 */
class AudioOutput {
public:
    using AudioCallback = std::function<void(float* buffer, int32_t numFrames)>;

    AudioOutput();
    ~AudioOutput();

    /**
     * Initialize audio output
     * @param sampleRate Sample rate (48000 or 96000)
     * @param bufferFrames Frames per buffer (larger = more latency, more stability)
     * @param callback Audio callback to fill buffers
     * @return true if successful
     */
    bool initialize(int32_t sampleRate, int32_t bufferFrames, AudioCallback callback);

    /**
     * Start audio playback
     */
    bool start();

    /**
     * Stop audio playback
     */
    void stop();

    /**
     * Pause audio playback
     */
    void pause();

    /**
     * Check if currently playing
     */
    bool isPlaying() const { return isPlaying_; }

    /**
     * Clean up resources
     */
    void shutdown();

private:
    /**
     * OpenSL ES callback (static)
     */
    static void audioCallback(SLAndroidSimpleBufferQueueItf bq, void* context);

    /**
     * Process audio buffer
     */
    void processAudio(SLAndroidSimpleBufferQueueItf bq);

    // OpenSL ES objects
    SLObjectItf engineObject_;
    SLEngineItf engineEngine_;
    SLObjectItf outputMixObject_;
    SLObjectItf playerObject_;
    SLPlayItf playerPlay_;
    SLAndroidSimpleBufferQueueItf playerBufferQueue_;

    // Audio parameters
    int32_t sampleRate_;
    int32_t bufferFrames_;
    AudioCallback callback_;
    
    // Double buffering (16-bit PCM for OpenSL ES)
    static constexpr int32_t NUM_BUFFERS = 8;
    int16_t* audioBuffers_[NUM_BUFFERS];  // PCM int16 buffers for OpenSL ES
    float* floatBuffer_;                   // Temp float buffer from callback
    int32_t currentBuffer_;
    
    bool isPlaying_;
    bool isInitialized_;
};

} // namespace spcmic

#endif // SPCMIC_AUDIO_OUTPUT_H

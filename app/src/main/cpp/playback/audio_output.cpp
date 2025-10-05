#include "audio_output.h"
#include <android/log.h>
#include <cstring>

#define LOG_TAG "AudioOutput"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace spcmic {

AudioOutput::AudioOutput()
    : engineObject_(nullptr)
    , engineEngine_(nullptr)
    , outputMixObject_(nullptr)
    , playerObject_(nullptr)
    , playerPlay_(nullptr)
    , playerBufferQueue_(nullptr)
    , sampleRate_(0)
    , bufferFrames_(0)
    , currentBuffer_(0)
    , isPlaying_(false)
    , isInitialized_(false)
    , floatBuffer_(nullptr) {
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        audioBuffers_[i] = nullptr;
    }
}

AudioOutput::~AudioOutput() {
    shutdown();
}

bool AudioOutput::initialize(int32_t sampleRate, int32_t bufferFrames, AudioCallback callback) {
    shutdown();

    sampleRate_ = sampleRate;
    bufferFrames_ = bufferFrames;
    callback_ = callback;

    // Allocate audio buffers (stereo int16 for OpenSL ES)
    for (int i = 0; i < NUM_BUFFERS; i++) {
        audioBuffers_[i] = new int16_t[bufferFrames * 2];
        memset(audioBuffers_[i], 0, bufferFrames * 2 * sizeof(int16_t));
    }
    
    // Allocate temp float buffer for callback
    floatBuffer_ = new float[bufferFrames * 2];

    // Create OpenSL ES engine
    SLresult result = slCreateEngine(&engineObject_, 0, nullptr, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to create OpenSL ES engine");
        return false;
    }

    result = (*engineObject_)->Realize(engineObject_, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to realize engine");
        return false;
    }

    result = (*engineObject_)->GetInterface(engineObject_, SL_IID_ENGINE, &engineEngine_);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to get engine interface");
        return false;
    }

    // Create output mix
    result = (*engineEngine_)->CreateOutputMix(engineEngine_, &outputMixObject_, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to create output mix");
        return false;
    }

    result = (*outputMixObject_)->Realize(outputMixObject_, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to realize output mix");
        return false;
    }

    // Configure audio source (stereo PCM 16-bit)
    // Note: Android's OpenSL ES doesn't support 24-bit despite it being in the spec
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
        NUM_BUFFERS
    };

    SLDataFormat_PCM format_pcm = {
        SL_DATAFORMAT_PCM,
        2,                                          // 2 channels (stereo)
        static_cast<SLuint32>(sampleRate * 1000),  // Sample rate in milliHz
        SL_PCMSAMPLEFORMAT_FIXED_16,                // 16-bit signed integer PCM
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
        SL_BYTEORDER_LITTLEENDIAN
    };

    SLDataSource audioSrc = {&loc_bufq, &format_pcm};

    // Configure audio sink
    SLDataLocator_OutputMix loc_outmix = {
        SL_DATALOCATOR_OUTPUTMIX,
        outputMixObject_
    };

    SLDataSink audioSnk = {&loc_outmix, nullptr};

    // Create audio player
    const SLInterfaceID ids[] = {SL_IID_BUFFERQUEUE};
    const SLboolean req[] = {SL_BOOLEAN_TRUE};

    result = (*engineEngine_)->CreateAudioPlayer(
        engineEngine_,
        &playerObject_,
        &audioSrc,
        &audioSnk,
        1,
        ids,
        req
    );

    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to create audio player");
        return false;
    }

    result = (*playerObject_)->Realize(playerObject_, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to realize player");
        return false;
    }

    // Get play interface
    result = (*playerObject_)->GetInterface(playerObject_, SL_IID_PLAY, &playerPlay_);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to get play interface");
        return false;
    }

    // Get buffer queue interface
    result = (*playerObject_)->GetInterface(playerObject_, SL_IID_BUFFERQUEUE, &playerBufferQueue_);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to get buffer queue interface");
        return false;
    }

    // Register callback
    result = (*playerBufferQueue_)->RegisterCallback(playerBufferQueue_, audioCallback, this);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to register callback");
        return false;
    }

    isInitialized_ = true;
    LOGD("AudioOutput initialized: %d Hz, %d frames/buffer", sampleRate, bufferFrames);
    
    return true;
}

bool AudioOutput::start() {
    if (!isInitialized_ || isPlaying_) {
        return false;
    }

    // Enqueue initial buffers
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (callback_) {
            // Get float samples from callback
            callback_(floatBuffer_, bufferFrames_);
            
            // Convert float to int16
            for (int32_t j = 0; j < bufferFrames_ * 2; j++) {
                float sample = floatBuffer_[j];
                // Clamp to [-1.0, 1.0]
                if (sample > 1.0f) sample = 1.0f;
                if (sample < -1.0f) sample = -1.0f;
                // Convert to int16
                audioBuffers_[i][j] = (int16_t)(sample * 32767.0f);
            }
        }
        
        (*playerBufferQueue_)->Enqueue(
            playerBufferQueue_,
            audioBuffers_[i],
            bufferFrames_ * 2 * sizeof(int16_t)
        );
    }

    // Start playback
    SLresult result = (*playerPlay_)->SetPlayState(playerPlay_, SL_PLAYSTATE_PLAYING);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to start playback");
        return false;
    }

    isPlaying_ = true;
    LOGD("Playback started");
    return true;
}

void AudioOutput::stop() {
    if (!isInitialized_ || !isPlaying_) {
        return;
    }

    (*playerPlay_)->SetPlayState(playerPlay_, SL_PLAYSTATE_STOPPED);
    (*playerBufferQueue_)->Clear(playerBufferQueue_);
    
    isPlaying_ = false;
    LOGD("Playback stopped");
}

void AudioOutput::pause() {
    if (!isInitialized_ || !isPlaying_) {
        return;
    }

    (*playerPlay_)->SetPlayState(playerPlay_, SL_PLAYSTATE_PAUSED);
    isPlaying_ = false;
    LOGD("Playback paused");
}

void AudioOutput::shutdown() {
    stop();

    // Destroy player
    if (playerObject_) {
        (*playerObject_)->Destroy(playerObject_);
        playerObject_ = nullptr;
        playerPlay_ = nullptr;
        playerBufferQueue_ = nullptr;
    }

    // Destroy output mix
    if (outputMixObject_) {
        (*outputMixObject_)->Destroy(outputMixObject_);
        outputMixObject_ = nullptr;
    }

    // Destroy engine
    if (engineObject_) {
        (*engineObject_)->Destroy(engineObject_);
        engineObject_ = nullptr;
        engineEngine_ = nullptr;
    }

    // Free buffers
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (audioBuffers_[i]) {
            delete[] audioBuffers_[i];
            audioBuffers_[i] = nullptr;
        }
    }
    
    if (floatBuffer_) {
        delete[] floatBuffer_;
        floatBuffer_ = nullptr;
    }

    isInitialized_ = false;
}

void AudioOutput::audioCallback(SLAndroidSimpleBufferQueueItf bq, void* context) {
    AudioOutput* output = static_cast<AudioOutput*>(context);
    output->processAudio(bq);
}

void AudioOutput::processAudio(SLAndroidSimpleBufferQueueItf bq) {
    if (!isPlaying_ || !callback_) {
        return;
    }

    // Get next buffer
    currentBuffer_ = (currentBuffer_ + 1) % NUM_BUFFERS;
    int16_t* buffer = audioBuffers_[currentBuffer_];

    // Fill float buffer with audio data from callback
    callback_(floatBuffer_, bufferFrames_);
    
    // Convert float to int16
    for (int32_t i = 0; i < bufferFrames_ * 2; i++) {
        float sample = floatBuffer_[i];
        // Clamp to [-1.0, 1.0]
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        // Convert to int16
        buffer[i] = (int16_t)(sample * 32767.0f);
    }

    // Enqueue buffer
    (*bq)->Enqueue(bq, buffer, bufferFrames_ * 2 * sizeof(int16_t));
}

} // namespace spcmic

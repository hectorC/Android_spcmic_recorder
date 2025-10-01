#include "android_audio_recorder.h"
#include <android/log.h>
#include <algorithm>
#include <cmath>

#define LOG_TAG "AndroidAudioRecorder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

AndroidAudioRecorder::AndroidAudioRecorder()
    : m_jvm(nullptr)
    , m_audioRecord(nullptr)
    , m_readMethod(nullptr)
    , m_startMethod(nullptr)
    , m_stopMethod(nullptr)
    , m_releaseMethod(nullptr)
    , m_sampleRate(48000)
    , m_channelCount(84)
    , m_bufferSize(0)
    , m_isRecording(false) {
    
    LOGI("AndroidAudioRecorder created");
}

AndroidAudioRecorder::~AndroidAudioRecorder() {
    stopRecording();
    
    if (m_jvm) {
        JNIEnv* env;
        if (m_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
            cleanupJNI(env);
        }
    }
    
    LOGI("AndroidAudioRecorder destroyed");
}

bool AndroidAudioRecorder::initialize(int sampleRate, int channelCount) {
    m_sampleRate = sampleRate;
    m_channelCount = channelCount;
    
    // Initialize channel levels array
    m_channelLevels.resize(m_channelCount, 0.0f);
    
    LOGI("AndroidAudioRecorder initialized for %d channels at %d Hz", channelCount, sampleRate);
    return true;
}

bool AndroidAudioRecorder::startRecording() {
    if (m_isRecording.load()) {
        LOGI("Already recording");
        return true;
    }
    
    LOGI("Starting Android AudioRecord-based recording");
    
    // Get current thread's JNI environment
    JNIEnv* env;
    if (!m_jvm) {
        // Get JavaVM from current thread
        jint result = JavaVM_GetJavaVM(&m_jvm);
        if (result != JNI_OK) {
            LOGE("Failed to get JavaVM");
            return false;
        }
    }
    
    if (m_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        LOGE("Failed to get JNI environment");
        return false;
    }
    
    if (!setupJNI(env)) {
        LOGE("Failed to setup JNI for AudioRecord");
        return false;
    }
    
    // Start recording thread
    m_isRecording.store(true);
    m_recordingThread = std::thread(&AndroidAudioRecorder::recordingThreadFunction, this);
    
    LOGI("Android AudioRecord recording started");
    return true;
}

void AndroidAudioRecorder::stopRecording() {
    if (!m_isRecording.load()) {
        return;
    }
    
    LOGI("Stopping Android AudioRecord recording");
    
    m_isRecording.store(false);
    
    if (m_recordingThread.joinable()) {
        m_recordingThread.join();
    }
    
    LOGI("Android AudioRecord recording stopped");
}

std::vector<float> AndroidAudioRecorder::getChannelLevels() {
    std::lock_guard<std::mutex> lock(m_levelsMutex);
    return m_channelLevels;
}

bool AndroidAudioRecorder::setupJNI(JNIEnv* env) {
    // For now, return true to avoid JNI complexity
    // We'll implement this step by step
    LOGI("JNI setup placeholder - will implement Android AudioRecord API");
    return true;
}

void AndroidAudioRecorder::cleanupJNI(JNIEnv* env) {
    // Cleanup JNI references
    if (m_audioRecord) {
        env->DeleteGlobalRef(m_audioRecord);
        m_audioRecord = nullptr;
    }
}

void AndroidAudioRecorder::recordingThreadFunction() {
    LOGI("Android AudioRecord thread started");
    
    // For now, just generate some test data to show the concept works
    int sampleCount = 0;
    
    while (m_isRecording.load()) {
        // Generate test channel levels (sine waves with different frequencies)
        {
            std::lock_guard<std::mutex> lock(m_levelsMutex);
            for (int i = 0; i < m_channelCount && i < (int)m_channelLevels.size(); i++) {
                // Generate different sine wave for each channel
                float phase = (sampleCount + i * 100) * 0.01f;
                m_channelLevels[i] = std::abs(std::sin(phase)) * 0.5f; // 0.0 to 0.5 range
            }
        }
        
        sampleCount++;
        
        // 50ms update rate (20 FPS)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    LOGI("Android AudioRecord thread finished");
}

void AndroidAudioRecorder::calculateChannelLevels(const int16_t* buffer, size_t samples) {
    // This will be implemented when we have real audio data
    // For now, it's not used since we're generating test data
}
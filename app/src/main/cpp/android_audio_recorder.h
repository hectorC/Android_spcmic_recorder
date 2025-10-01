#pragma once

#include <jni.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>

class AndroidAudioRecorder {
public:
    AndroidAudioRecorder();
    ~AndroidAudioRecorder();
    
    bool initialize(int sampleRate, int channelCount);
    bool startRecording();
    void stopRecording();
    
    std::vector<float> getChannelLevels();
    
private:
    // JNI references
    JavaVM* m_jvm;
    jobject m_audioRecord;
    jmethodID m_readMethod;
    jmethodID m_startMethod;
    jmethodID m_stopMethod;
    jmethodID m_releaseMethod;
    
    // Audio parameters
    int m_sampleRate;
    int m_channelCount;
    int m_bufferSize;
    
    // Monitoring
    std::atomic<bool> m_isRecording;
    std::thread m_recordingThread;
    std::vector<float> m_channelLevels;
    std::mutex m_levelsMutex;
    
    // Recording thread function
    void recordingThreadFunction();
    void calculateChannelLevels(const int16_t* buffer, size_t samples);
    
    // JNI helpers
    bool setupJNI(JNIEnv* env);
    void cleanupJNI(JNIEnv* env);
};
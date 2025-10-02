#pragma once

#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include "usb_audio_interface.h"
#include "wav_writer.h"

class MultichannelRecorder {
public:
    MultichannelRecorder(USBAudioInterface* audioInterface);
    ~MultichannelRecorder();
    
    bool startRecording(const std::string& outputPath);
    bool stopRecording();
    bool isRecording() const { return m_isRecording.load(); }
    
    // Monitoring mode - read audio for level meters without recording
    bool startMonitoring();
    void stopMonitoring();
    bool isMonitoring() const { return m_isMonitoring.load(); }
    
    // Get real-time channel levels for UI display
    std::vector<float> getChannelLevels();
    
    // Get recording statistics
    size_t getTotalSamplesRecorded() const { return m_totalSamples; }
    double getRecordingDuration() const;
    
private:
    USBAudioInterface* m_audioInterface;
    WAVWriter* m_wavWriter;
    
    std::atomic<bool> m_isRecording;
    std::atomic<bool> m_isMonitoring;
    std::thread m_recordingThread;
    std::thread m_monitoringThread;
    
    // Channel level monitoring
    std::vector<float> m_channelLevels;
    std::mutex m_levelsMutex;
    
    // Recording statistics
    size_t m_totalSamples;
    std::chrono::high_resolution_clock::time_point m_startTime;
    
    // Recording parameters
    static const size_t DEFAULT_BUFFER_SIZE = 8192;  // Bytes
    static const int CHANNEL_COUNT = 84;
    static const int SAMPLE_RATE = 48000;
    static const int BYTES_PER_SAMPLE = 3;  // 24-bit
    
    // Recording thread function
    void recordingThreadFunction();
    
    // Monitoring thread function (for level meters without recording)
    void monitoringThreadFunction();
    
    // Audio processing
    void processAudioBuffer(const uint8_t* buffer, size_t bufferSize);
    void calculateChannelLevels(const uint8_t* buffer, size_t bufferSize);
    
    // Utility functions
    int32_t extract24BitSample(const uint8_t* data);
    float normalizeLevel(int32_t sample);

    size_t m_bufferSize;
};
#pragma once

#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <functional>
#include <condition_variable>
#include "usb_audio_interface.h"
#include "wav_writer.h"
#include "lock_free_ring_buffer.h"

class MultichannelRecorder {
public:
    MultichannelRecorder(USBAudioInterface* audioInterface);
    ~MultichannelRecorder();
    
    // Monitoring mode (USB streaming + audio processing, no file writing)
    bool startMonitoring(float gainDb = 0.0f);
    bool stopMonitoring();
    bool isMonitoring() const { return m_isMonitoring.load(); }
    
    // Recording mode (monitoring + file writing)
    bool startRecording(const std::string& outputPath, float gainDb = 0.0f);
    bool startRecordingWithFd(int fd, const std::string& displayPath, float gainDb = 0.0f);
    bool startRecordingFromMonitoring(const std::string& outputPath);
    bool startRecordingFromMonitoringWithFd(int fd, const std::string& displayPath);
    
    // Stop both monitoring and recording
    bool stopRecording();
    bool isRecording() const { return m_isRecording.load(); }
    
    bool hasClipped() const { return m_clipDetected.load(); }
    void resetClipIndicator();
    
    // Gain and level metering
    void setGain(float gainDb);
    float getPeakLevel() const { return m_peakLevel.load(); }
    
    // Get recording statistics
    size_t getTotalSamplesRecorded() const { return static_cast<size_t>(m_totalSamples.load(std::memory_order_relaxed)); }
    double getRecordingDuration() const;
    
private:
    USBAudioInterface* m_audioInterface;
    WAVWriter* m_wavWriter;
    
    std::atomic<bool> m_isMonitoring;  // USB streaming + audio processing active
    std::atomic<bool> m_isRecording;   // File writing active (implies monitoring)
    std::thread m_recordingThread;
    std::thread m_diskWriteThread;
    
    // Lock-free ring buffer for decoupling USB reads from disk writes
    LockFreeRingBuffer* m_ringBuffer;
    static const size_t RING_BUFFER_SIZE = 4 * 1024 * 1024;  // 4 MB ring buffer (~3.3 seconds @ 1.2 MB/s)
    std::atomic<bool> m_diskThreadRunning;
    std::condition_variable m_diskThreadCV;
    std::mutex m_diskThreadMutex;
    
    // Recording statistics
    std::atomic<uint64_t> m_totalSamples;
    std::chrono::high_resolution_clock::time_point m_startTime;
    int m_sampleRate;
    std::atomic<bool> m_clipDetected;
    
    // Gain and level metering
    std::atomic<float> m_peakLevel;
    float m_gainLinear;              // Current gain (smoothly interpolated by audio thread)
    std::atomic<float> m_targetGainLinear;  // Target gain set by UI thread
    float m_gainSmoothingCoeff;      // Smoothing coefficient (calculated from sample rate)
    
    // Recording parameters
    static const size_t DEFAULT_BUFFER_SIZE = 8192;  // Bytes
    static const int CHANNEL_COUNT = 84;
    static const int BYTES_PER_SAMPLE = 3;  // 24-bit
    
    // Recording thread function
    void recordingThreadFunction();
    
    // Disk write thread function (separate from USB reading)
    void diskWriteThreadFunction();
    
    // Audio processing
    void processAudioBuffer(const uint8_t* buffer, size_t bufferSize);
    
    // Utility functions
    int32_t extract24BitSample(const uint8_t* data);
    float normalizeLevel(int32_t sample);

    size_t m_bufferSize;

    bool startRecordingInternal(
        const std::string& destinationLabel,
        const std::function<bool(WAVWriter*)>& openWriter);
};
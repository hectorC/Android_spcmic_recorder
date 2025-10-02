#include "multichannel_recorder.h"
#include <android/log.h>
#include <chrono>
#include <algorithm>
#include <cmath>

#define LOG_TAG "MultichannelRecorder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

MultichannelRecorder::MultichannelRecorder(USBAudioInterface* audioInterface)
    : m_audioInterface(audioInterface)
    , m_wavWriter(nullptr)
    , m_isRecording(false)
    , m_isMonitoring(false)
    , m_totalSamples(0)
    , m_sampleRate(audioInterface ? audioInterface->getEffectiveSampleRateRounded() : 48000)
    , m_clipDetected(false)
    , m_bufferSize(DEFAULT_BUFFER_SIZE) {
    
    // Initialize channel levels array
    m_channelLevels.resize(CHANNEL_COUNT, 0.0f);
    
    LOGI("MultichannelRecorder created for %d channels", CHANNEL_COUNT);
}

MultichannelRecorder::~MultichannelRecorder() {
    stopRecording();
    stopMonitoring();
    
    if (m_wavWriter) {
        delete m_wavWriter;
        m_wavWriter = nullptr;
    }
    
    LOGI("MultichannelRecorder destroyed");
}

bool MultichannelRecorder::startRecording(const std::string& outputPath) {
    if (m_isRecording.load()) {
        LOGE("Recording already in progress");
        return false;
    }
    
    if (!m_audioInterface) {
        LOGE("No audio interface available");
        return false;
    }
    
    // Start USB audio streaming first so the device can negotiate its actual sample rate
    if (!m_audioInterface->startStreaming()) {
        LOGE("Failed to start USB audio streaming");
        return false;
    }

    int effectiveRate = m_audioInterface->getEffectiveSampleRateRounded();
    if (effectiveRate > 0) {
        m_sampleRate = effectiveRate;
    }

    LOGI("Starting recording to: %s (sampleRate=%d Hz)", outputPath.c_str(), m_sampleRate);
    
    // Create WAV writer using the negotiated sample rate
    if (m_wavWriter) {
        delete m_wavWriter;
    }
    
    m_wavWriter = new WAVWriter();
    if (!m_wavWriter->open(outputPath, m_sampleRate, CHANNEL_COUNT, BYTES_PER_SAMPLE * 8)) {
        LOGE("Failed to create WAV file: %s", outputPath.c_str());
        delete m_wavWriter;
        m_wavWriter = nullptr;
        m_audioInterface->stopStreaming();
        return false;
    }

    const size_t frameSize = static_cast<size_t>(CHANNEL_COUNT) * BYTES_PER_SAMPLE;
    size_t recommendedSize = m_audioInterface->getRecommendedBufferSize();
    if (recommendedSize == 0) {
        recommendedSize = DEFAULT_BUFFER_SIZE;
    }
    if (recommendedSize < frameSize) {
        recommendedSize = frameSize;
    }
    if (recommendedSize % frameSize != 0) {
        recommendedSize = ((recommendedSize + frameSize - 1) / frameSize) * frameSize;
    }
    m_bufferSize = recommendedSize;

    LOGI("Recording buffer size configured: %zu bytes (frameSize=%zu)", m_bufferSize, frameSize);
    
    // Reset recording state
    m_totalSamples = 0;
    m_startTime = std::chrono::high_resolution_clock::now();
    m_clipDetected.store(false);
    
    // Start recording thread
    m_isRecording.store(true);
    m_recordingThread = std::thread(&MultichannelRecorder::recordingThreadFunction, this);
    
    LOGI("Recording started successfully");
    return true;
}

bool MultichannelRecorder::stopRecording() {
    if (!m_isRecording.load()) {
        return true;
    }
    
    LOGI("Stopping recording");
    
    // Signal recording thread to stop
    m_isRecording.store(false);
    
    // Wait for recording thread to finish
    if (m_recordingThread.joinable()) {
        m_recordingThread.join();
    }
    
    // Stop USB audio streaming
    if (m_audioInterface) {
        m_audioInterface->stopStreaming();
    }
    
    // Close WAV file
    if (m_wavWriter) {
        m_wavWriter->close();
        delete m_wavWriter;
        m_wavWriter = nullptr;
    }
    
    LOGI("Recording stopped. Total samples: %zu", m_totalSamples);
    return true;
}

bool MultichannelRecorder::startMonitoring() {
    if (m_isMonitoring.load()) {
        LOGI("Already monitoring");
        return true;
    }
    
    if (!m_audioInterface) {
        LOGE("No audio interface available for monitoring");
        return false;
    }
    
    LOGI("Starting audio monitoring for level meters");
    
    // Start USB audio streaming
    if (!m_audioInterface->startStreaming()) {
        LOGE("Failed to start USB audio streaming for monitoring");
        return false;
    }
    
    // Set monitoring flag
    m_isMonitoring.store(true);
    m_clipDetected.store(false);
    
    // Start monitoring thread
    m_monitoringThread = std::thread(&MultichannelRecorder::monitoringThreadFunction, this);
    
    LOGI("Audio monitoring started");
    return true;
}

void MultichannelRecorder::stopMonitoring() {
    if (!m_isMonitoring.load()) {
        return;
    }
    
    LOGI("Stopping audio monitoring");
    
    // Signal monitoring thread to stop
    m_isMonitoring.store(false);
    
    // Wait for monitoring thread to finish
    if (m_monitoringThread.joinable()) {
        m_monitoringThread.join();
    }
    
    // Stop USB audio streaming (only if not recording)
    if (!m_isRecording.load() && m_audioInterface) {
        m_audioInterface->stopStreaming();
    }
    
    LOGI("Audio monitoring stopped");
}

void MultichannelRecorder::recordingThreadFunction() {
    LOGI("Recording thread started");
    
    if (m_bufferSize == 0) {
        m_bufferSize = DEFAULT_BUFFER_SIZE;
    }

    std::vector<uint8_t> buffer(m_bufferSize);
    
    while (m_isRecording.load()) {
        // Read audio data from USB interface
    size_t bytesRead = m_audioInterface->readAudioData(buffer.data(), buffer.size());
        
        if (bytesRead > 0) {
            // Process the audio buffer
            processAudioBuffer(buffer.data(), bytesRead);
            
            // Write to WAV file
            if (m_wavWriter) {
                m_wavWriter->writeData(buffer.data(), bytesRead);
            }
            
            // Update total samples
            size_t samplesInBuffer = bytesRead / (CHANNEL_COUNT * BYTES_PER_SAMPLE);
            m_totalSamples += samplesInBuffer;
            
            // Calculate channel levels for UI
            calculateChannelLevels(buffer.data(), bytesRead);
        } else {
            // No data available, small delay to prevent busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    LOGI("Recording thread finished");
}

void MultichannelRecorder::monitoringThreadFunction() {
    LOGI("Monitoring thread started");
    
    size_t monitoringBufferSize = m_bufferSize;
    if (monitoringBufferSize == 0) {
        monitoringBufferSize = DEFAULT_BUFFER_SIZE;
    }
    std::vector<uint8_t> buffer(monitoringBufferSize);
    
    while (m_isMonitoring.load()) {
        // Read audio data from USB interface (same as recording, but don't save to file)
    size_t bytesRead = m_audioInterface->readAudioData(buffer.data(), buffer.size());
        
        if (bytesRead > 0) {
            // Only calculate channel levels for UI (don't save to file)
            calculateChannelLevels(buffer.data(), bytesRead);
        } else {
            // No data available, small delay to prevent busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    LOGI("Monitoring thread finished");
}

void MultichannelRecorder::processAudioBuffer(const uint8_t* buffer, size_t bufferSize) {
    // This function can be used for real-time audio processing
    // such as filtering, gain adjustment, etc.
    
    // For now, we just pass the data through unchanged
    // but this is where you could add:
    // - Real-time filtering
    // - Automatic gain control
    // - Noise reduction
    // - Format conversion
}

void MultichannelRecorder::calculateChannelLevels(const uint8_t* buffer, size_t bufferSize) {
    std::lock_guard<std::mutex> lock(m_levelsMutex);
    
    // Calculate RMS levels for each channel
    std::vector<double> channelSums(CHANNEL_COUNT, 0.0);
    size_t samplesPerChannel = bufferSize / (CHANNEL_COUNT * BYTES_PER_SAMPLE);
    
    if (samplesPerChannel == 0) {
        return;
    }
    
    // Process interleaved 24-bit samples
    const uint8_t* samplePtr = buffer;
    
    for (size_t sample = 0; sample < samplesPerChannel; ++sample) {
        for (int channel = 0; channel < CHANNEL_COUNT; ++channel) {
            // Extract 24-bit sample
            int32_t sampleValue = extract24BitSample(samplePtr);
            samplePtr += BYTES_PER_SAMPLE;
            
            if (m_isRecording.load()) {
                constexpr int32_t CLIP_THRESHOLD = 0x7FFFFF;
                if (sampleValue >= CLIP_THRESHOLD || sampleValue <= -CLIP_THRESHOLD) {
                    m_clipDetected.store(true, std::memory_order_relaxed);
                }
            }

            // Accumulate squared values for RMS calculation
            double normalizedSample = static_cast<double>(sampleValue) / 8388608.0; // 2^23
            channelSums[channel] += normalizedSample * normalizedSample;
        }
    }
    
    // Calculate RMS levels and update channel levels array
    for (int channel = 0; channel < CHANNEL_COUNT; ++channel) {
        double rms = std::sqrt(channelSums[channel] / samplesPerChannel);
        m_channelLevels[channel] = static_cast<float>(rms);
    }
}

int32_t MultichannelRecorder::extract24BitSample(const uint8_t* data) {
    // Extract 24-bit sample (little-endian) and sign-extend to 32-bit
    int32_t sample = (data[0]) | (data[1] << 8) | (data[2] << 16);
    
    // Sign extend from 24-bit to 32-bit
    if (sample & 0x800000) {
        sample |= 0xFF000000;
    }
    
    return sample;
}

float MultichannelRecorder::normalizeLevel(int32_t sample) {
    // Normalize 24-bit sample to 0.0-1.0 range
    return std::abs(static_cast<float>(sample)) / 8388608.0f; // 2^23
}

std::vector<float> MultichannelRecorder::getChannelLevels() {
    std::lock_guard<std::mutex> lock(m_levelsMutex);
    return m_channelLevels;
}

double MultichannelRecorder::getRecordingDuration() const {
    if (m_totalSamples == 0 || m_sampleRate <= 0) {
        return 0.0;
    }
    
    return static_cast<double>(m_totalSamples) / static_cast<double>(m_sampleRate);
}

void MultichannelRecorder::resetClipIndicator() {
    m_clipDetected.store(false);
}
#include "multichannel_recorder.h"
#include <android/log.h>
#include <chrono>
#include <algorithm>
#include <cmath>

#define LOG_TAG "MultichannelRecorder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOG_FATAL_IF(cond, ...) \
    do { \
        if (cond) { \
            __android_log_assert(#cond, LOG_TAG, __VA_ARGS__); \
        } \
    } while (false)

MultichannelRecorder::MultichannelRecorder(USBAudioInterface* audioInterface)
    : m_audioInterface(audioInterface)
    , m_wavWriter(nullptr)
    , m_isRecording(false)
    , m_totalSamples(0)
    , m_sampleRate(audioInterface ? audioInterface->getEffectiveSampleRateRounded() : 48000)
    , m_clipDetected(false)
    , m_bufferSize(DEFAULT_BUFFER_SIZE)
    , m_ringBuffer(nullptr)
    , m_diskThreadRunning(false)
    , m_peakLevel(0.0f)
    , m_gainLinear(1.0f) {
    
    LOGI("MultichannelRecorder created for %d channels", CHANNEL_COUNT);
}

MultichannelRecorder::~MultichannelRecorder() {
    // First, ensure recording is stopped and threads are signaled.
    if (m_isRecording.load()) {
        m_isRecording.store(false);
    }
    if (m_diskThreadRunning.load()) {
        m_diskThreadRunning.store(false);
        m_diskThreadCV.notify_one();
    }

    // Now, safely join the threads.
    if (m_recordingThread.joinable()) {
        m_recordingThread.join();
    }
    if (m_diskWriteThread.joinable()) {
        m_diskWriteThread.join();
    }
    
    // Now it's safe to clean up other resources.
    if (m_wavWriter) {
        // Closing the writer handles the final flush and header update
        m_wavWriter->close();
        delete m_wavWriter;
        m_wavWriter = nullptr;
    }
    
    if (m_ringBuffer) {
        delete m_ringBuffer;
        m_ringBuffer = nullptr;
    }
    
    LOGI("MultichannelRecorder destroyed");
}

bool MultichannelRecorder::startRecording(const std::string& outputPath, float gainDb) {
    // Set gain before starting recording
    setGain(gainDb);
    
    return startRecordingInternal(
        outputPath,
        [this, &outputPath](WAVWriter* writer) {
            return writer->open(outputPath, m_sampleRate, CHANNEL_COUNT, BYTES_PER_SAMPLE * 8);
        }
    );
}

bool MultichannelRecorder::startRecordingWithFd(int fd, const std::string& displayPath, float gainDb) {
    // Set gain before starting recording
    setGain(gainDb);
    
    return startRecordingInternal(
        displayPath,
        [this, fd](WAVWriter* writer) {
            return writer->openFromFd(fd, m_sampleRate, CHANNEL_COUNT, BYTES_PER_SAMPLE * 8);
        }
    );
}

bool MultichannelRecorder::stopRecording() {
    if (!m_isRecording.load()) {
        // If not recording, there's nothing to do.
        // This prevents trying to stop a recorder that's already been stopped.
        return true;
    }
    
    LOGI("Stopping recording threads and flushing data...");
    
    // 1. Signal recording thread to stop.
    m_isRecording.store(false);
    
    // 2. Wait for the USB reading thread to finish its current loop and exit.
    if (m_recordingThread.joinable()) {
        m_recordingThread.join();
    }
    
    // 3. Signal the disk write thread that no more data is coming.
    m_diskThreadRunning.store(false);
    m_diskThreadCV.notify_one(); // Wake it up in case it's waiting on the condition variable.
    
    // 4. Wait for the disk write thread to finish flushing the ring buffer and exit.
    if (m_diskWriteThread.joinable()) {
        m_diskWriteThread.join();
    }
    
    // 5. Stop the underlying USB audio streaming.
    if (m_audioInterface) {
        m_audioInterface->stopStreaming();
    }
    
    // 6. Close the WAV file, which writes the final header.
    if (m_wavWriter) {
        m_wavWriter->close();
        // The object itself will be deleted by the owner (JNI layer).
    }
    
    LOGI("Recording stopped and flushed. Total samples: %zu", m_totalSamples);
    return true;
}

void MultichannelRecorder::recordingThreadFunction() {
    LOGI("USB reading thread started");
    
    if (m_bufferSize == 0) {
        m_bufferSize = DEFAULT_BUFFER_SIZE;
    }

    std::vector<uint8_t> buffer(m_bufferSize);
    size_t consecutiveEmptyReads = 0;
    size_t totalBytesRead = 0;
    size_t bufferOverflows = 0;
    
    while (m_isRecording.load()) {
        // Read audio data from USB interface
        size_t bytesRead = m_audioInterface->readAudioData(buffer.data(), buffer.size());
        
        LOG_FATAL_IF(bytesRead > buffer.size(),
                     "bytesRead=%zu exceeds staging buffer=%zu", bytesRead, buffer.size());

        if (bytesRead > 0) {
            consecutiveEmptyReads = 0;
            totalBytesRead += bytesRead;
            
            // Process the audio buffer (level meters, clip detection)
            processAudioBuffer(buffer.data(), bytesRead);

            // Update total samples
            size_t samplesInBuffer = bytesRead / (CHANNEL_COUNT * BYTES_PER_SAMPLE);
            m_totalSamples += samplesInBuffer;
            
            // Write to ring buffer (lock-free, non-blocking)
            if (m_ringBuffer) {
                const size_t availableSpace = m_ringBuffer->getAvailableSpace();
                if (bytesRead > availableSpace) {
                    LOGE("Ring buffer pressure: incoming=%zu, space=%zu (overflow #%zu)",
                         bytesRead, availableSpace, bufferOverflows + 1);
                }

                size_t bytesWritten = m_ringBuffer->write(buffer.data(), bytesRead);
                
                if (bytesWritten < bytesRead) {
                    // Ring buffer is full - this is a critical error indicating disk I/O can't keep up
                    bufferOverflows++;
                    if (bufferOverflows % 10 == 1) {  // Log every 10th overflow to avoid spam
                        LOGE("Ring buffer overflow! Disk I/O can't keep up. Lost %zu bytes (overflow #%zu)",
                             bytesRead - bytesWritten, bufferOverflows);
                    }
                }
                
                // Wake up disk write thread if it's waiting
                m_diskThreadCV.notify_one();
            }
        } else {
            consecutiveEmptyReads++;
            
            // Only log if we get many consecutive empty reads (indicates potential problem)
            if (consecutiveEmptyReads == 100) {
                LOGE("Warning: 100 consecutive empty USB reads. Total bytes read so far: %zu", totalBytesRead);
            }
            
            // Small yield to scheduler - we're ready to read more data immediately
            // but don't want to completely starve other threads
            std::this_thread::yield();
        }
    }
    
    if (bufferOverflows > 0) {
        LOGE("USB reading thread finished. Total buffer overflows: %zu", bufferOverflows);
    } else {
        LOGI("USB reading thread finished cleanly. Total bytes read: %zu", totalBytesRead);
    }
}

void MultichannelRecorder::diskWriteThreadFunction() {
    LOGI("Disk write thread started");
    
    // Use a larger buffer for disk writes to amortize I/O overhead
    const size_t DISK_WRITE_BUFFER_SIZE = 256 * 1024;  // 256 KB (~200ms of audio)
    std::vector<uint8_t> diskBuffer(DISK_WRITE_BUFFER_SIZE);
    
    size_t totalBytesWritten = 0;
    size_t writeCount = 0;
    
    while (m_diskThreadRunning.load()) {
        size_t bytesAvailable = m_ringBuffer ? m_ringBuffer->getAvailableBytes() : 0;
        
        if (bytesAvailable > 0) {
            // Read from ring buffer
            size_t toRead = std::min(bytesAvailable, DISK_WRITE_BUFFER_SIZE);
            size_t bytesRead = m_ringBuffer->read(diskBuffer.data(), toRead);
            
            if (bytesRead > 0) {
                // Write to WAV file
                if (m_wavWriter) {
                    m_wavWriter->writeData(diskBuffer.data(), bytesRead);
                    totalBytesWritten += bytesRead;
                    writeCount++;
                    
                    // Periodically log write statistics
                    if (writeCount % 100 == 0) {
                        size_t bufferFill = m_ringBuffer->getAvailableBytes();
                        double fillPercent = (bufferFill * 100.0) / m_ringBuffer->getCapacity();
                        LOGI("Disk write stats: %zu writes, %zu MB written, ring buffer %.1f%% full",
                             writeCount, totalBytesWritten / (1024 * 1024), fillPercent);
                    }
                }
            }
        } else {
            // Ring buffer is empty - wait for data with a timeout
            // This prevents busy-waiting while still being responsive
            std::unique_lock<std::mutex> lock(m_diskThreadMutex);
            m_diskThreadCV.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                return !m_diskThreadRunning.load() || 
                       (m_ringBuffer && m_ringBuffer->getAvailableBytes() > 0);
            });
        }
    }
    
    // Flush remaining data in ring buffer before exiting
    if (m_ringBuffer) {
        size_t remainingBytes = m_ringBuffer->getAvailableBytes();
        if (remainingBytes > 0) {
            LOGI("Flushing %zu remaining bytes from ring buffer", remainingBytes);
            
            while (remainingBytes > 0) {
                size_t toRead = std::min(remainingBytes, DISK_WRITE_BUFFER_SIZE);
                size_t bytesRead = m_ringBuffer->read(diskBuffer.data(), toRead);
                
                if (bytesRead > 0 && m_wavWriter) {
                    m_wavWriter->writeData(diskBuffer.data(), bytesRead);
                    totalBytesWritten += bytesRead;
                }
                
                remainingBytes = m_ringBuffer->getAvailableBytes();
            }
        }
    }
    
    LOGI("Disk write thread finished. Total writes: %zu, Total bytes: %zu MB",
         writeCount, totalBytesWritten / (1024 * 1024));
}

void MultichannelRecorder::processAudioBuffer(const uint8_t* buffer, size_t bufferSize) {
    // This function processes audio in real-time:
    // 1. Apply gain to all channels
    // 2. Track peak level across all channels for metering
    // 3. Modify buffer in-place before it goes to disk
    
    if (bufferSize == 0 || buffer == nullptr) {
        return;
    }
    
    // Process samples in-place (gain application + level detection)
    const size_t frameSize = CHANNEL_COUNT * BYTES_PER_SAMPLE;
    const size_t numFrames = bufferSize / frameSize;
    
    // Track peak level for this buffer
    float bufferPeak = 0.0f;
    
    // Process each complete frame
    uint8_t* mutableBuffer = const_cast<uint8_t*>(buffer);
    for (size_t frame = 0; frame < numFrames; ++frame) {
        const size_t frameOffset = frame * frameSize;
        
        // Process each channel in this frame
        for (size_t ch = 0; ch < CHANNEL_COUNT; ++ch) {
            const size_t sampleOffset = frameOffset + (ch * BYTES_PER_SAMPLE);
            
            // Extract 24-bit sample
            int32_t sample = extract24BitSample(mutableBuffer + sampleOffset);
            
            // Apply gain (multiply by linear gain factor)
            if (m_gainLinear != 1.0f) {
                // Convert to float, apply gain, convert back to int32
                float sampleFloat = static_cast<float>(sample);
                sampleFloat *= m_gainLinear;
                
                // Check for clipping BEFORE clamping (detect if gain caused clipping)
                constexpr float MAX_24BIT = 8388607.0f;  // 2^23 - 1
                constexpr float MIN_24BIT = -8388608.0f; // -2^23
                if (sampleFloat >= MAX_24BIT || sampleFloat <= MIN_24BIT) {
                    m_clipDetected.store(true, std::memory_order_relaxed);
                }
                
                // Clamp to 24-bit range to prevent overflow
                sampleFloat = std::max(MIN_24BIT, std::min(MAX_24BIT, sampleFloat));
                
                sample = static_cast<int32_t>(sampleFloat);
                
                // Write modified sample back to buffer (24-bit little-endian)
                mutableBuffer[sampleOffset + 0] = static_cast<uint8_t>(sample & 0xFF);
                mutableBuffer[sampleOffset + 1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
                mutableBuffer[sampleOffset + 2] = static_cast<uint8_t>((sample >> 16) & 0xFF);
            }
            
            // Track peak level (after gain application)
            float level = normalizeLevel(sample);
            if (level > bufferPeak) {
                bufferPeak = level;
            }
        }
    }
    
    // Update peak level atomically (for UI polling)
    // Use exponential decay: new_peak = max(current_sample_peak, old_peak * 0.95)
    // This gives smooth meter falloff
    float currentPeak = m_peakLevel.load(std::memory_order_relaxed);
    float decayedPeak = currentPeak * 0.95f;
    float newPeak = std::max(bufferPeak, decayedPeak);
    m_peakLevel.store(newPeak, std::memory_order_relaxed);
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

double MultichannelRecorder::getRecordingDuration() const {
    if (m_totalSamples == 0 || m_sampleRate <= 0) {
        return 0.0;
    }
    
    return static_cast<double>(m_totalSamples) / static_cast<double>(m_sampleRate);
}

void MultichannelRecorder::resetClipIndicator() {
    m_clipDetected.store(false);
}

void MultichannelRecorder::setGain(float gainDb) {
    // Convert dB to linear gain: linear = 10^(dB/20)
    // Clamp gainDb to safe range [0, 64] dB
    gainDb = std::max(0.0f, std::min(64.0f, gainDb));
    m_gainLinear = std::pow(10.0f, gainDb / 20.0f);
    
    LOGI("Gain set to %.1f dB (linear: %.2f)", gainDb, m_gainLinear);
}

bool MultichannelRecorder::startRecordingInternal(
    const std::string& destinationLabel,
    const std::function<bool(WAVWriter*)>& openWriter) {

    if (m_isRecording.load()) {
        LOGE("Recording already in progress");
        return false;
    }

    if (!m_audioInterface) {
        LOGE("No audio interface available");
        return false;
    }

    if (!m_audioInterface->startStreaming()) {
        LOGE("Failed to start USB audio streaming");
        return false;
    }

    int effectiveRate = m_audioInterface->getEffectiveSampleRateRounded();
    if (effectiveRate > 0) {
        m_sampleRate = effectiveRate;
    }

    LOGI("Starting recording to: %s (sampleRate=%d Hz)", destinationLabel.c_str(), m_sampleRate);

    m_wavWriter = new WAVWriter();
    if (!openWriter(m_wavWriter)) {
        LOGE("Failed to prepare WAV destination: %s", destinationLabel.c_str());
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

    m_totalSamples = 0;
    m_startTime = std::chrono::high_resolution_clock::now();
    m_clipDetected.store(false);

    // Create ring buffer for decoupling USB reads from disk writes
    m_ringBuffer = new LockFreeRingBuffer(RING_BUFFER_SIZE);
    LOGI("Created ring buffer: %zu MB (%zu bytes)", 
         RING_BUFFER_SIZE / (1024 * 1024), RING_BUFFER_SIZE);

    // Start disk write thread first
    m_diskThreadRunning.store(true);
    m_diskWriteThread = std::thread(&MultichannelRecorder::diskWriteThreadFunction, this);
    
    // Start USB reading thread
    m_isRecording.store(true);
    m_recordingThread = std::thread(&MultichannelRecorder::recordingThreadFunction, this);

    LOGI("Recording started successfully with dual-thread architecture");
    return true;
}
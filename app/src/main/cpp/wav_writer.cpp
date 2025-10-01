#include "wav_writer.h"
#include <android/log.h>
#include <cstring>

#define LOG_TAG "WAVWriter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

WAVWriter::WAVWriter()
    : m_sampleRate(0)
    , m_channels(0)
    , m_bitsPerSample(0)
    , m_bytesPerSample(0)
    , m_blockAlign(0)
    , m_byteRate(0)
    , m_dataSize(0)
    , m_dataChunkPos(0) {
}

WAVWriter::~WAVWriter() {
    close();
}

bool WAVWriter::open(const std::string& filename, int sampleRate, int channels, int bitsPerSample) {
    if (m_file.is_open()) {
        LOGE("WAV file already open");
        return false;
    }
    
    LOGI("Opening WAV file: %s (%dHz, %dch, %dbit)", 
         filename.c_str(), sampleRate, channels, bitsPerSample);
    
    m_filename = filename;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_bitsPerSample = bitsPerSample;
    m_bytesPerSample = bitsPerSample / 8;
    m_blockAlign = m_channels * m_bytesPerSample;
    m_byteRate = m_sampleRate * m_blockAlign;
    m_dataSize = 0;
    
    // Open file for binary writing
    m_file.open(filename, std::ios::binary | std::ios::out);
    if (!m_file.is_open()) {
        LOGE("Failed to open WAV file: %s", filename.c_str());
        return false;
    }
    
    // Write initial WAV header
    if (!writeHeader()) {
        LOGE("Failed to write WAV header");
        m_file.close();
        return false;
    }
    
    LOGI("WAV file opened successfully");
    return true;
}

bool WAVWriter::writeData(const uint8_t* data, size_t size) {
    if (!m_file.is_open()) {
        LOGE("WAV file not open");
        return false;
    }
    
    if (!data || size == 0) {
        return true;
    }
    
    // Write audio data
    m_file.write(reinterpret_cast<const char*>(data), size);
    if (m_file.fail()) {
        LOGE("Failed to write audio data");
        return false;
    }
    
    m_dataSize += size;
    return true;
}

void WAVWriter::close() {
    if (!m_file.is_open()) {
        return;
    }
    
    LOGI("Closing WAV file: %s (wrote %zu bytes)", m_filename.c_str(), m_dataSize);
    
    // Update WAV header with final file size
    updateHeader();
    
    m_file.close();
    
    LOGI("WAV file closed successfully");
}

bool WAVWriter::writeHeader() {
    WAVHeader header;
    initializeHeader(header);
    
    // Remember position of data chunk for later update
    m_dataChunkPos = m_file.tellp() + std::streampos(sizeof(WAVHeader) - sizeof(uint32_t));
    
    // Write header
    m_file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (m_file.fail()) {
        LOGE("Failed to write WAV header");
        return false;
    }
    
    return true;
}

bool WAVWriter::updateHeader() {
    if (!m_file.is_open()) {
        return false;
    }
    
    // Save current position
    std::streampos currentPos = m_file.tellp();
    
    // Update RIFF chunk size (file size - 8)
    uint32_t riffSize = sizeof(WAVHeader) - 8 + m_dataSize;
    m_file.seekp(4);
    m_file.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    
    // Update data chunk size
    m_file.seekp(m_dataChunkPos);
    m_file.write(reinterpret_cast<const char*>(&m_dataSize), sizeof(m_dataSize));
    
    // Restore position
    m_file.seekp(currentPos);
    
    if (m_file.fail()) {
        LOGE("Failed to update WAV header");
        return false;
    }
    
    return true;
}

void WAVWriter::initializeHeader(WAVHeader& header) {
    // Initialize WAV header structure
    memset(&header, 0, sizeof(header));
    
    // RIFF chunk
    memcpy(header.riffID, "RIFF", 4);
    header.riffSize = sizeof(WAVHeader) - 8; // Will be updated when closing
    memcpy(header.waveID, "WAVE", 4);
    
    // Format chunk
    memcpy(header.formatID, "fmt ", 4);
    header.formatSize = 16; // PCM format chunk size
    header.audioFormat = 1; // PCM
    header.numChannels = m_channels;
    header.sampleRate = m_sampleRate;
    header.byteRate = m_byteRate;
    header.blockAlign = m_blockAlign;
    header.bitsPerSample = m_bitsPerSample;
    
    // Data chunk header
    memcpy(header.dataID, "data", 4);
    header.dataSize = 0; // Will be updated when closing
    
    LOGI("WAV header initialized:");
    LOGI("  Sample Rate: %d Hz", header.sampleRate);
    LOGI("  Channels: %d", header.numChannels);
    LOGI("  Bits per Sample: %d", header.bitsPerSample);
    LOGI("  Byte Rate: %d", header.byteRate);
    LOGI("  Block Align: %d", header.blockAlign);
}
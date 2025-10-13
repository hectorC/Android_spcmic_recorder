#pragma once

#include <string>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <sys/types.h>

class WAVWriter {
public:
    WAVWriter();
    ~WAVWriter();
    
    bool open(const std::string& filename, int sampleRate, int channels, int bitsPerSample);
    bool openFromFd(int fd, int sampleRate, int channels, int bitsPerSample);
    bool writeData(const uint8_t* data, size_t size);
    void close();
    
    bool isOpen() const { return m_file != nullptr; }
    size_t getBytesWritten() const { return m_dataSize; }
    
private:
    FILE* m_file;
    int m_ownedFd;
    std::string m_filename;
    
    // WAV format parameters
    int m_sampleRate;
    int m_channels;
    int m_bitsPerSample;
    int m_bytesPerSample;
    int m_blockAlign;
    int m_byteRate;
    
    // File tracking
    size_t m_dataSize;
    off_t m_dataChunkPos;
    
    // WAV file structure
    struct WAVHeader {
        // RIFF chunk
        char riffID[4];           // "RIFF"
        uint32_t riffSize;        // File size - 8
        char waveID[4];           // "WAVE"
        
        // Format chunk
        char formatID[4];         // "fmt "
        uint32_t formatSize;      // Format chunk size (16 for PCM)
        uint16_t audioFormat;     // Audio format (1 for PCM)
        uint16_t numChannels;     // Number of channels
        uint32_t sampleRate;      // Sample rate
        uint32_t byteRate;        // Byte rate
        uint16_t blockAlign;      // Block align
        uint16_t bitsPerSample;   // Bits per sample
        
        // Data chunk header
        char dataID[4];           // "data"
        uint32_t dataSize;        // Data size
    } __attribute__((packed));
    
    static constexpr off_t RIFF_SIZE_OFFSET = offsetof(WAVHeader, riffSize);
    static constexpr off_t DATA_SIZE_OFFSET = offsetof(WAVHeader, dataSize);

    bool writeHeader();
    bool updateHeader();
    void initializeHeader(WAVHeader& header);
    void initializeFormat(int sampleRate, int channels, int bitsPerSample);
    void resetState();
};
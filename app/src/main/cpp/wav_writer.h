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
    static constexpr uint32_t MAX_UINT32 = 0xFFFFFFFFu;
    static constexpr uint32_t DS64_CHUNK_SIZE = 28u;

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
    uint64_t m_dataSize;
    uint64_t m_totalFrames;
    off_t m_dataSizePos;
    off_t m_ds64ChunkPos;
    off_t m_ds64SizePos;
    off_t m_ds64DataPos;

    bool writeHeader();
    bool updateHeader();
    void initializeFormat(int sampleRate, int channels, int bitsPerSample);
    void resetState();

    bool writeFourCC(const char* fourcc);
    bool writeUint16(uint16_t value);
    bool writeUint32(uint32_t value);
    bool writeUint64(uint64_t value);
    bool writeZeros(size_t count);
};
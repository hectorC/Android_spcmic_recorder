#ifndef SPCMIC_WAV_FILE_READER_H
#define SPCMIC_WAV_FILE_READER_H

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace spcmic {

/**
 * WAV file header structures
 */
#pragma pack(push, 1)
struct WavHeader {
    char riff[4];           // "RIFF"
    uint32_t fileSize;      // File size - 8
    char wave[4];           // "WAVE"
};

struct WavFmtChunk {
    char fmt[4];            // "fmt "
    uint32_t chunkSize;     // 16 for PCM
    uint16_t audioFormat;   // 1 = PCM, 3 = IEEE float
    uint16_t numChannels;   // 84 for our recordings
    uint32_t sampleRate;    // 48000 or 96000
    uint32_t byteRate;      // sampleRate * numChannels * bitsPerSample/8
    uint16_t blockAlign;    // numChannels * bitsPerSample/8
    uint16_t bitsPerSample; // 24 or 32
};

struct WavDataChunk {
    char data[4];           // "data"
    uint32_t dataSize;      // Size of audio data
};
#pragma pack(pop)

/**
 * 84-channel WAV file reader for SPCMic recordings
 * Supports streaming playback with buffered reading
 */
class WavFileReader {
public:
    WavFileReader();
    ~WavFileReader();

    /**
     * Open a 84-channel WAV file for reading
     * @param filePath Absolute path to WAV file
     * @return true if file opened successfully
     */
    bool open(const std::string& filePath);

    /**
     * Close the current file
     */
    void close();

    /**
     * Read interleaved audio samples
     * @param buffer Output buffer (must be numFrames * 84 * sizeof(float))
     * @param numFrames Number of frames to read
     * @return Number of frames actually read
     */
    int32_t read(float* buffer, int32_t numFrames);

    /**
     * Seek to a specific frame position
     * @param framePosition Frame number (0-based)
     * @return true if seek successful
     */
    bool seek(int64_t framePosition);

    /**
     * Get current playback position in frames
     */
    int64_t getPosition() const { return currentFrame_; }

    /**
     * Get total duration in frames
     */
    int64_t getTotalFrames() const { return totalFrames_; }

    /**
     * Get total duration in seconds
     */
    double getDurationSeconds() const;

    /**
     * Get file properties
     */
    int32_t getNumChannels() const { return numChannels_; }
    int32_t getSampleRate() const { return sampleRate_; }
    int32_t getBitsPerSample() const { return bitsPerSample_; }
    bool isOpen() const { return fileHandle_ != nullptr; }

private:
    /**
     * Read and validate WAV header
     */
    bool readHeader();

    /**
     * Convert 24-bit packed samples to float
     */
    void convert24BitToFloat(const uint8_t* src, float* dst, int32_t numSamples);

    /**
     * Convert 32-bit int samples to float
     */
    void convert32BitToFloat(const int32_t* src, float* dst, int32_t numSamples);
         void convert16BitToFloat(const int16_t* src, float* dst, int32_t numSamples);

    FILE* fileHandle_;
    int64_t dataStartOffset_;
    int64_t dataSize_;
    int64_t currentFrame_;
    int64_t totalFrames_;
    
    int32_t numChannels_;
    int32_t sampleRate_;
    int32_t bitsPerSample_;
    int32_t bytesPerFrame_;
    
    // Read buffer for raw file data
    std::vector<uint8_t> readBuffer_;
};

} // namespace spcmic

#endif // SPCMIC_WAV_FILE_READER_H

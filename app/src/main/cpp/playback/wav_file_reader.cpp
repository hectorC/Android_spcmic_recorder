#include "wav_file_reader.h"
#include <android/log.h>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <errno.h>

#define LOG_TAG "WavFileReader"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace spcmic {

WavFileReader::WavFileReader()
    : fileHandle_(nullptr)
    , dataStartOffset_(0)
    , dataSize_(0)
    , currentFrame_(0)
    , totalFrames_(0)
    , numChannels_(0)
    , sampleRate_(0)
    , bitsPerSample_(0)
    , bytesPerFrame_(0) {
}

WavFileReader::~WavFileReader() {
    close();
}

bool WavFileReader::open(const std::string& filePath) {
    close();

    fileHandle_ = fopen(filePath.c_str(), "rb");
    if (!fileHandle_) {
        LOGE("Failed to open file: %s", filePath.c_str());
        return false;
    }

    if (!readHeader()) {
        LOGE("Invalid WAV file format");
        close();
        return false;
    }

    LOGD("Opened WAV file: %d channels, %d Hz, %d-bit, %lld frames (%.2f seconds)",
         numChannels_, sampleRate_, bitsPerSample_, (long long)totalFrames_, getDurationSeconds());

    return true;
}

bool WavFileReader::openFromFd(int fd, const std::string& displayPath) {
    close();

    if (fd < 0) {
        LOGE("Invalid file descriptor for %s", displayPath.c_str());
        return false;
    }

    int dupFd = dup(fd);
    if (dupFd < 0) {
        LOGE("Failed to dup file descriptor for %s: %s", displayPath.c_str(), strerror(errno));
        return false;
    }

    fileHandle_ = fdopen(dupFd, "rb");
    if (!fileHandle_) {
        LOGE("Failed to fdopen descriptor for %s: %s", displayPath.c_str(), strerror(errno));
        ::close(dupFd);
        return false;
    }

    if (!readHeader()) {
        LOGE("Invalid WAV file format for descriptor (%s)", displayPath.c_str());
        close();
        return false;
    }

    LOGD("Opened WAV descriptor %s: %d channels, %d Hz, %d-bit, %lld frames (%.2f seconds)",
         displayPath.c_str(), numChannels_, sampleRate_, bitsPerSample_, (long long)totalFrames_, getDurationSeconds());

    return true;
}

void WavFileReader::close() {
    if (fileHandle_) {
        fclose(fileHandle_);
        fileHandle_ = nullptr;
    }
    currentFrame_ = 0;
}

bool WavFileReader::readHeader() {
    // Read RIFF header
    WavHeader header;
    if (fread(&header, sizeof(header), 1, fileHandle_) != 1) {
        LOGE("Failed to read WAV header");
        return false;
    }

    if (strncmp(header.riff, "RIFF", 4) != 0 || strncmp(header.wave, "WAVE", 4) != 0) {
        LOGE("Not a valid WAV file");
        return false;
    }

    // Read fmt chunk
    WavFmtChunk fmtChunk;
    if (fread(&fmtChunk, sizeof(fmtChunk), 1, fileHandle_) != 1) {
        LOGE("Failed to read fmt chunk");
        return false;
    }

    if (strncmp(fmtChunk.fmt, "fmt ", 4) != 0) {
        LOGE("Invalid fmt chunk");
        return false;
    }

    // Skip any extra fmt chunk data
    if (fmtChunk.chunkSize > 16) {
        fseek(fileHandle_, fmtChunk.chunkSize - 16, SEEK_CUR);
    }

    numChannels_ = fmtChunk.numChannels;
    sampleRate_ = fmtChunk.sampleRate;
    bitsPerSample_ = fmtChunk.bitsPerSample;
    bytesPerFrame_ = numChannels_ * (bitsPerSample_ / 8);

    // Validate format
    if (fmtChunk.audioFormat != 1 && fmtChunk.audioFormat != 3) {
        LOGE("Unsupported audio format: %d (only PCM/float supported)", fmtChunk.audioFormat);
        return false;
    }

    // Find data chunk (skip other chunks)
    char chunkId[4];
    uint32_t chunkSize;
    
    while (fread(chunkId, 4, 1, fileHandle_) == 1) {
        if (fread(&chunkSize, 4, 1, fileHandle_) != 1) {
            LOGE("Failed to read chunk size");
            return false;
        }

        if (strncmp(chunkId, "data", 4) == 0) {
            dataSize_ = chunkSize;
            dataStartOffset_ = ftell(fileHandle_);
            totalFrames_ = dataSize_ / bytesPerFrame_;
            
            // Allocate read buffer (8KB worth of frames)
            int32_t bufferFrames = std::max(8192 / bytesPerFrame_, 256);
            readBuffer_.resize(bufferFrames * bytesPerFrame_);
            
            return true;
        }

        // Skip this chunk
        fseek(fileHandle_, chunkSize, SEEK_CUR);
    }

    LOGE("No data chunk found");
    return false;
}

int32_t WavFileReader::read(float* buffer, int32_t numFrames) {
    if (!fileHandle_ || currentFrame_ >= totalFrames_) {
        return 0;
    }

    // Clamp to available frames
    int32_t framesToRead = std::min(numFrames, (int32_t)(totalFrames_ - currentFrame_));

    if (framesToRead <= 0) {
        return 0;
    }

    int32_t bytesToRead = framesToRead * bytesPerFrame_;
    if (readBuffer_.size() < static_cast<size_t>(bytesToRead)) {
        readBuffer_.resize(bytesToRead);
    }

    if (bitsPerSample_ == 24) {
        // Read 24-bit data
        int32_t bytesRead = fread(readBuffer_.data(), 1, bytesToRead, fileHandle_);
        int32_t framesRead = bytesRead / bytesPerFrame_;

        if (framesRead > 0) {
            convert24BitToFloat(readBuffer_.data(), buffer, framesRead * numChannels_);
            currentFrame_ += framesRead;
        }
        return framesRead;

    } else if (bitsPerSample_ == 32) {
        // Read 32-bit data
        int32_t bytesRead = fread(readBuffer_.data(), 1, bytesToRead, fileHandle_);
        int32_t framesRead = bytesRead / bytesPerFrame_;

        if (framesRead > 0) {
            convert32BitToFloat(reinterpret_cast<int32_t*>(readBuffer_.data()), buffer, framesRead * numChannels_);
            currentFrame_ += framesRead;
        }
        return framesRead;

    } else if (bitsPerSample_ == 16) {
        int32_t bytesRead = fread(readBuffer_.data(), 1, bytesToRead, fileHandle_);
        int32_t framesRead = bytesRead / bytesPerFrame_;

        if (framesRead > 0) {
            convert16BitToFloat(reinterpret_cast<int16_t*>(readBuffer_.data()), buffer, framesRead * numChannels_);
            currentFrame_ += framesRead;
        }
        return framesRead;

    } else {
        LOGE("Unsupported bit depth: %d", bitsPerSample_);
        return 0;
    }
}

bool WavFileReader::seek(int64_t framePosition) {
    if (!fileHandle_) {
        return false;
    }

    framePosition = std::max(static_cast<int64_t>(0), std::min(framePosition, totalFrames_));
    int64_t byteOffset = dataStartOffset_ + (framePosition * bytesPerFrame_);
    
    if (fseek(fileHandle_, byteOffset, SEEK_SET) != 0) {
        LOGE("Seek failed to frame %lld", (long long)framePosition);
        return false;
    }

    currentFrame_ = framePosition;
    return true;
}

double WavFileReader::getDurationSeconds() const {
    if (sampleRate_ == 0) return 0.0;
    return (double)totalFrames_ / (double)sampleRate_;
}

void WavFileReader::convert24BitToFloat(const uint8_t* src, float* dst, int32_t numSamples) {
    const float scale = 1.0f / 8388608.0f; // 2^23
    
    // Log first few samples for debugging
    static bool logged = false;
    
    for (int32_t i = 0; i < numSamples; i++) {
        // Read 3 bytes as little-endian 24-bit signed integer
        int32_t sample = (src[2] << 16) | (src[1] << 8) | src[0];
        
        // Sign extend from 24-bit to 32-bit
        if (sample & 0x800000) {
            sample |= 0xFF000000;
        }
        
        dst[i] = (float)sample * scale;
        
        // Log first 10 samples once
        if (!logged && i < 10) {
            LOGD("Sample %d: bytes[%02X %02X %02X] -> int32=%d -> float=%.6f", 
                 i, src[0], src[1], src[2], sample, dst[i]);
        }
        
        src += 3;
    }
    
    if (!logged) {
        logged = true;
    }
}

void WavFileReader::convert32BitToFloat(const int32_t* src, float* dst, int32_t numSamples) {
    const float scale = 1.0f / 2147483648.0f; // 2^31
    
    for (int32_t i = 0; i < numSamples; i++) {
        dst[i] = (float)src[i] * scale;
    }
}

void WavFileReader::convert16BitToFloat(const int16_t* src, float* dst, int32_t numSamples) {
    const float scale = 1.0f / 32768.0f;

    for (int32_t i = 0; i < numSamples; ++i) {
        dst[i] = static_cast<float>(src[i]) * scale;
    }
}

} // namespace spcmic

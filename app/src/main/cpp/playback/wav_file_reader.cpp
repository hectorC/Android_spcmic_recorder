#include "wav_file_reader.h"
#include <android/log.h>
#include <cstring>
#include <algorithm>
#include <vector>
#include <limits>
#include <unistd.h>
#include <errno.h>

#define LOG_TAG "WavFileReader"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

inline uint64_t readUint64LE(const uint8_t* data) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | static_cast<uint64_t>(data[i]);
    }
    return value;
}

}

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
    // Read RIFF/RF64 header
    WavHeader header;
    if (fread(&header, sizeof(header), 1, fileHandle_) != 1) {
        LOGE("Failed to read WAV header");
        return false;
    }

    const bool isRf64 = (strncmp(header.riff, "RF64", 4) == 0);
    if (!isRf64 && strncmp(header.riff, "RIFF", 4) != 0) {
        LOGE("Not a valid RIFF/RF64 file");
        return false;
    }
    if (strncmp(header.wave, "WAVE", 4) != 0) {
        LOGE("Not a WAVE file");
        return false;
    }

    uint64_t ds64DataSize = 0;
    uint64_t ds64SampleCount = 0;
    bool hasDs64 = false;
    bool fmtFound = false;

    auto readUint16LE = [](const uint8_t* data) {
        return static_cast<uint16_t>(data[0] | (data[1] << 8));
    };

    auto readUint32LE = [](const uint8_t* data) {
        return static_cast<uint32_t>(
            (static_cast<uint32_t>(data[0])      ) |
            (static_cast<uint32_t>(data[1]) <<  8) |
            (static_cast<uint32_t>(data[2]) << 16) |
            (static_cast<uint32_t>(data[3]) << 24));
    };

    // Find data chunk (skip other chunks)
    char chunkId[4];
    uint32_t chunkSize = 0;

    while (fread(chunkId, 1, 4, fileHandle_) == 4) {
        if (fread(&chunkSize, 4, 1, fileHandle_) != 1) {
            LOGE("Failed to read chunk size");
            return false;
        }

        if (strncmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                LOGE("Invalid fmt chunk size: %u", chunkSize);
                return false;
            }

            std::vector<uint8_t> buffer(chunkSize);
            if (fread(buffer.data(), 1, chunkSize, fileHandle_) != chunkSize) {
                LOGE("Failed to read fmt chunk payload");
                return false;
            }

            const uint16_t audioFormat = readUint16LE(buffer.data());
            numChannels_ = static_cast<int32_t>(readUint16LE(buffer.data() + 2));
            sampleRate_ = static_cast<int32_t>(readUint32LE(buffer.data() + 4));
            const uint32_t byteRate = readUint32LE(buffer.data() + 8);
            const uint16_t blockAlign = readUint16LE(buffer.data() + 12);
            bitsPerSample_ = static_cast<int32_t>(readUint16LE(buffer.data() + 14));

            (void)byteRate;      // not currently used
            (void)blockAlign;    // not currently used

            if (audioFormat != 1 && audioFormat != 3) {
                LOGE("Unsupported audio format: %u (only PCM/float supported)", audioFormat);
                return false;
            }

            bytesPerFrame_ = numChannels_ * std::max<int32_t>(1, bitsPerSample_ / 8);
            fmtFound = true;

            if (chunkSize & 1) {
                if (fseeko(fileHandle_, 1, SEEK_CUR) != 0) {
                    LOGE("Failed to skip fmt padding");
                    return false;
                }
            }
            continue;
        }

        if (strncmp(chunkId, "ds64", 4) == 0) {
            if (chunkSize < 28) {
                LOGE("Invalid ds64 chunk size: %u", chunkSize);
                return false;
            }

            std::vector<uint8_t> buffer(chunkSize);
            if (fread(buffer.data(), 1, chunkSize, fileHandle_) != chunkSize) {
                LOGE("Failed to read ds64 chunk");
                return false;
            }

            ds64DataSize = readUint64LE(buffer.data() + 8);
            ds64SampleCount = readUint64LE(buffer.data() + 16);
            hasDs64 = true;

            if (chunkSize & 1) {
                if (fseeko(fileHandle_, 1, SEEK_CUR) != 0) {
                    LOGE("Failed to skip ds64 padding");
                    return false;
                }
            }
            continue;
        }

        if (strncmp(chunkId, "data", 4) == 0) {
            if (!fmtFound) {
                LOGE("Encountered data chunk before fmt chunk");
                return false;
            }

            uint64_t dataSize64 = chunkSize;
            if (hasDs64) {
                if (chunkSize == 0xFFFFFFFFu || ds64DataSize > dataSize64) {
                    dataSize64 = ds64DataSize;
                }
            }

            dataStartOffset_ = ftello(fileHandle_);
            if (dataStartOffset_ < 0) {
                LOGE("Failed to obtain data offset");
                return false;
            }

            dataSize_ = static_cast<int64_t>(dataSize64);

            uint64_t frames64 = 0;
            if (hasDs64 && ds64SampleCount > 0) {
                frames64 = ds64SampleCount;
            } else if (bytesPerFrame_ > 0) {
                frames64 = dataSize64 / static_cast<uint64_t>(bytesPerFrame_);
            }
            totalFrames_ = static_cast<int64_t>(std::min<uint64_t>(frames64, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));

            // Allocate read buffer (8KB worth of frames)
            int32_t clampedBytesPerFrame = std::max(bytesPerFrame_, 1);
            int32_t bufferFrames = std::max(8192 / clampedBytesPerFrame, 256);
            readBuffer_.resize(static_cast<size_t>(bufferFrames) * static_cast<size_t>(clampedBytesPerFrame));

            return true;
        }

        // Skip this chunk (plus optional padding)
        off_t skipAmount = static_cast<off_t>(chunkSize);
        if (fseeko(fileHandle_, skipAmount, SEEK_CUR) != 0) {
            LOGE("Failed to skip chunk %c%c%c%c", chunkId[0], chunkId[1], chunkId[2], chunkId[3]);
            return false;
        }
        if (chunkSize & 1) {
            if (fseeko(fileHandle_, 1, SEEK_CUR) != 0) {
                LOGE("Failed to skip chunk padding");
                return false;
            }
        }
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
    int64_t byteOffset = dataStartOffset_ + (framePosition * static_cast<int64_t>(bytesPerFrame_));

    if (fseeko(fileHandle_, static_cast<off_t>(byteOffset), SEEK_SET) != 0) {
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

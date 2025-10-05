#include "matrix_convolver/ir_loader.h"
#include <android/asset_manager.h>
#include <android/log.h>
#include <cstring>
#include <vector>
#include <algorithm>

#define LOG_TAG "IRLoader"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kNumInputChannels = 84;

inline uint16_t ReadLE16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

inline uint32_t ReadLE32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
}

} // namespace

namespace spcmic {

IRLoader::IRLoader()
    : assetManager_(nullptr) {}

void IRLoader::setAssetManager(AAssetManager* manager) {
    assetManager_ = manager;
}

std::string IRLoader::buildBinauralAssetName(int sampleRateHz) {
    if (sampleRateHz >= 96000) {
        return "impulse_responses/binaural_96k.wav";
    }
    return "impulse_responses/binaural_48k.wav";
}

bool IRLoader::loadBinaural(int sampleRateHz, MatrixImpulseResponse& outIR) {
    if (!assetManager_) {
        LOGW("Asset manager not set. Cannot load IR.");
        return false;
    }

    const std::string assetName = buildBinauralAssetName(sampleRateHz);
    if (!loadFromAsset(assetName, sampleRateHz, outIR)) {
        LOGE("Failed to load binaural IR asset: %s", assetName.c_str());
        return false;
    }

    LOGD("Loaded binaural IR: %s (IR length=%d, channels=%d)",
         assetName.c_str(), outIR.irLength, outIR.numInputChannels);
    return true;
}

bool IRLoader::loadFromAsset(const std::string& assetName,
                             int expectedSampleRate,
                             MatrixImpulseResponse& outIR) {
    AAsset* asset = AAssetManager_open(assetManager_, assetName.c_str(), AASSET_MODE_BUFFER);
    if (!asset) {
        LOGE("Unable to open asset: %s", assetName.c_str());
        return false;
    }

    const off_t assetLength = AAsset_getLength(asset);
    if (assetLength <= 0) {
        LOGE("Asset %s has invalid length", assetName.c_str());
        AAsset_close(asset);
        return false;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(assetLength));
    int64_t totalRead = 0;
    while (totalRead < assetLength) {
        const int64_t read = AAsset_read(asset,
                                         buffer.data() + totalRead,
                                         static_cast<size_t>(assetLength - totalRead));
        if (read <= 0) {
            LOGE("Failed to read asset %s (read=%lld, total=%lld)", assetName.c_str(),
                 static_cast<long long>(read), static_cast<long long>(totalRead));
            AAsset_close(asset);
            return false;
        }
        totalRead += read;
    }

    AAsset_close(asset);

    if (buffer.size() < 44) {
        LOGE("Asset %s too small to be a valid WAV file", assetName.c_str());
        return false;
    }

    const uint8_t* data = buffer.data();
    if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        LOGE("Asset %s is not a RIFF/WAVE file", assetName.c_str());
        return false;
    }

    size_t offset = 12; // Skip RIFF header
    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    uint32_t dataSize = 0;
    size_t dataOffset = 0;

    while (offset + 8 <= buffer.size()) {
        const char* chunkId = reinterpret_cast<const char*>(data + offset);
        const uint32_t chunkSize = ReadLE32(data + offset + 4);
        offset += 8;

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16 || offset + chunkSize > buffer.size()) {
                LOGE("Invalid fmt chunk in asset %s", assetName.c_str());
                return false;
            }
            audioFormat = ReadLE16(data + offset);
            numChannels = ReadLE16(data + offset + 2);
            sampleRate = ReadLE32(data + offset + 4);
            bitsPerSample = ReadLE16(data + offset + 14);

            offset += chunkSize;
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            dataOffset = offset;
            offset += chunkSize;
            break; // we only care about first data chunk
        } else {
            // Skip unknown chunk
            offset += chunkSize;
        }

        // Chunks are padded to even sizes
        if (chunkSize % 2 == 1) {
            ++offset;
        }
    }

    if (numChannels != 2) {
        LOGE("Expected stereo IR asset. Channels=%u", numChannels);
        return false;
    }

    if (sampleRate == 0) {
        LOGE("Invalid sample rate in asset %s", assetName.c_str());
        return false;
    }

    if (expectedSampleRate != sampleRate) {
        LOGW("IR sample rate %u differs from requested %d. Continuing.", sampleRate, expectedSampleRate);
    }

    if (bitsPerSample != 32 && bitsPerSample != 24) {
        LOGE("Unsupported bit depth %u in asset %s", bitsPerSample, assetName.c_str());
        return false;
    }

    if (dataSize == 0 || dataOffset + dataSize > buffer.size()) {
        LOGE("Invalid data chunk in asset %s", assetName.c_str());
        return false;
    }

    const int bytesPerSample = bitsPerSample / 8;
    const size_t totalFrames = dataSize / (numChannels * bytesPerSample);
    if (totalFrames == 0) {
        LOGE("No audio frames in IR asset %s", assetName.c_str());
        return false;
    }

    if (totalFrames % kNumInputChannels != 0) {
        LOGE("IR asset %s does not contain %d evenly sized impulse responses (frames=%zu)",
             assetName.c_str(), kNumInputChannels, totalFrames);
        return false;
    }

    const size_t irLength = totalFrames / kNumInputChannels;
    const uint8_t* samples = data + dataOffset;

    std::vector<float> leftChannel(totalFrames);
    std::vector<float> rightChannel(totalFrames);

    if (bitsPerSample == 32 && audioFormat == 3) {
        // 32-bit IEEE float
        for (size_t frame = 0; frame < totalFrames; ++frame) {
            const size_t base = frame * 2 * bytesPerSample;
            float leftValue;
            float rightValue;
            std::memcpy(&leftValue, samples + base, sizeof(float));
            std::memcpy(&rightValue, samples + base + bytesPerSample, sizeof(float));
            leftChannel[frame] = leftValue;
            rightChannel[frame] = rightValue;
        }
    } else {
        const float scale = (bitsPerSample == 32) ? (1.0f / 2147483648.0f)
                                                 : (1.0f / 8388608.0f);
        for (size_t frame = 0; frame < totalFrames; ++frame) {
            const size_t base = frame * 2 * bytesPerSample;

            int32_t leftInt = 0;
            int32_t rightInt = 0;
            if (bitsPerSample == 32) {
                leftInt = static_cast<int32_t>(ReadLE32(samples + base));
                rightInt = static_cast<int32_t>(ReadLE32(samples + base + bytesPerSample));
            } else { // 24-bit
                const uint8_t* leftPtr = samples + base;
                leftInt = (leftPtr[2] << 16) | (leftPtr[1] << 8) | leftPtr[0];
                if (leftInt & 0x800000) {
                    leftInt |= 0xFF000000;
                }

                const uint8_t* rightPtr = samples + base + bytesPerSample;
                rightInt = (rightPtr[2] << 16) | (rightPtr[1] << 8) | rightPtr[0];
                if (rightInt & 0x800000) {
                    rightInt |= 0xFF000000;
                }
            }

            leftChannel[frame] = static_cast<float>(leftInt) * scale;
            rightChannel[frame] = static_cast<float>(rightInt) * scale;
        }
    }

    outIR.sampleRate = static_cast<int>(sampleRate);
    outIR.irLength = static_cast<int>(irLength);
    outIR.numInputChannels = kNumInputChannels;
    outIR.leftEar.resize(static_cast<size_t>(kNumInputChannels) * irLength);
    outIR.rightEar.resize(static_cast<size_t>(kNumInputChannels) * irLength);

    for (int channel = 0; channel < kNumInputChannels; ++channel) {
        const size_t srcOffset = static_cast<size_t>(channel) * irLength;
        const size_t dstOffset = srcOffset;
        std::copy_n(leftChannel.begin() + srcOffset, irLength, outIR.leftEar.begin() + dstOffset);
        std::copy_n(rightChannel.begin() + srcOffset, irLength, outIR.rightEar.begin() + dstOffset);
    }

    return outIR.isValid();
}

} // namespace spcmic

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

std::string IRLoader::buildAssetName(IRPreset preset, int sampleRateHz) {
    const char* base = nullptr;
    switch (preset) {
        case IRPreset::Binaural:
            base = "binaural";
            break;
        case IRPreset::Ortf:
            base = "ortf";
            break;
        case IRPreset::Xy:
            base = "xy";
            break;
        case IRPreset::ThirdOrderAmbisonic:
            base = "3oa";
            break;
        default:
            base = "binaural";
            break;
    }

    const char* rateSuffix = (sampleRateHz >= 96000) ? "96k" : "48k";
    std::string assetName = "impulse_responses/";
    assetName += base;
    assetName += "_";
    assetName += rateSuffix;
    assetName += ".wav";
    return assetName;
}

bool IRLoader::loadPreset(IRPreset preset, int sampleRateHz, MatrixImpulseResponse& outIR) {
    if (!assetManager_) {
        LOGW("Asset manager not set. Cannot load IR.");
        return false;
    }

    const std::string assetName = buildAssetName(preset, sampleRateHz);
    if (!loadFromAsset(assetName, sampleRateHz, outIR)) {
        LOGE("Failed to load IR asset: %s", assetName.c_str());
        return false;
    }

    LOGD("Loaded IR: %s (IR length=%d, inputs=%d, outputs=%d)",
         assetName.c_str(), outIR.irLength, outIR.numInputChannels, outIR.numOutputChannels);
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

    if (numChannels == 0) {
        LOGE("IR asset %s has zero channels", assetName.c_str());
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

    std::vector<std::vector<float>> channelData(numChannels, std::vector<float>(totalFrames));

    if (bitsPerSample == 32 && audioFormat == 3) {
        const size_t frameStride = static_cast<size_t>(numChannels) * bytesPerSample;
        for (size_t frame = 0; frame < totalFrames; ++frame) {
            const size_t base = frame * frameStride;
            for (uint16_t ch = 0; ch < numChannels; ++ch) {
                float value;
                std::memcpy(&value, samples + base + ch * bytesPerSample, sizeof(float));
                channelData[ch][frame] = value;
            }
        }
    } else {
        const float scale = (bitsPerSample == 32) ? (1.0f / 2147483648.0f)
                                                 : (bitsPerSample == 24 ? (1.0f / 8388608.0f)
                                                                       : (1.0f / 32768.0f));
        const size_t frameStride = static_cast<size_t>(numChannels) * bytesPerSample;
        for (size_t frame = 0; frame < totalFrames; ++frame) {
            const size_t base = frame * frameStride;
            for (uint16_t ch = 0; ch < numChannels; ++ch) {
                const uint8_t* samplePtr = samples + base + ch * bytesPerSample;
                int32_t value = 0;
                if (bitsPerSample == 32) {
                    value = static_cast<int32_t>(ReadLE32(samplePtr));
                } else if (bitsPerSample == 24) {
                    value = (samplePtr[2] << 16) | (samplePtr[1] << 8) | samplePtr[0];
                    if (value & 0x800000) {
                        value |= 0xFF000000;
                    }
                } else { // 16-bit
                    value = static_cast<int16_t>(samplePtr[0] | (samplePtr[1] << 8));
                }
                channelData[ch][frame] = static_cast<float>(value) * scale;
            }
        }
    }

    outIR.sampleRate = static_cast<int>(sampleRate);
    outIR.irLength = static_cast<int>(irLength);
    outIR.numInputChannels = kNumInputChannels;
    outIR.numOutputChannels = numChannels;
    outIR.impulseData.resize(static_cast<size_t>(numChannels) * kNumInputChannels * irLength);

    for (uint16_t outChannel = 0; outChannel < numChannels; ++outChannel) {
        const auto& channelVector = channelData[outChannel];
        for (int inChannel = 0; inChannel < kNumInputChannels; ++inChannel) {
            const size_t srcOffset = static_cast<size_t>(inChannel) * irLength;
            const size_t dstOffset = (static_cast<size_t>(outChannel) * kNumInputChannels +
                                      static_cast<size_t>(inChannel)) * irLength;
            std::copy_n(channelVector.begin() + srcOffset, irLength, outIR.impulseData.begin() + dstOffset);
        }
    }

    return outIR.isValid();
}

} // namespace spcmic

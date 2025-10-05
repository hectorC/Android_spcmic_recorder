#include "playback_engine.h"
#include <android/log.h>
#include "wav_writer.h"
#include <android/asset_manager.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

#define LOG_TAG "PlaybackEngine"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace spcmic {

namespace {

constexpr const char* kCacheFileName = "playback_cache.wav";

std::string JoinPath(const std::string& dir, const std::string& file) {
    if (dir.empty()) {
        return file;
    }

    const char lastChar = dir.back();
    if (lastChar == '/' || lastChar == '\\') {
        return dir + file;
    }

    return dir + "/" + file;
}

} // namespace

PlaybackEngine::PlaybackEngine()
    : state_(State::IDLE)
    , playbackCompleted_(false)
    , impulseResponseLoaded_(false)
    , assetManager_(nullptr)
    , sourceFilePath_()
    , preRenderedFilePath_()
    , preRenderCacheDir_()
    , preRenderedSourcePath_()
    , preRenderedReady_(false)
    , usePreRendered_(false)
    , sourceSampleRate_(0)
    , sourceBitsPerSample_(0)
    , sourceNumChannels_(0)
    , playbackGainLinear_(1.0f)
    , preRenderProgress_(0)
    , preRenderInProgress_(false) {
    
    // Allocate input buffer for 84 channels
    inputBuffer_.resize(BUFFER_FRAMES * 84);
    stereoBuffer_.resize(BUFFER_FRAMES * 2);
    stereo24Buffer_.resize(BUFFER_FRAMES * 2 * 3);
    
    // Create audio output
    audioOutput_ = std::make_unique<AudioOutput>();

    matrixConvolver_.reset();
}

PlaybackEngine::~PlaybackEngine() {
    stop();
}

bool PlaybackEngine::loadFile(const std::string& filePath) {
    audioOutput_->stop();

    clearPreRenderedState();
    sourceFilePath_ = filePath;

    // Stop current playback outside of the file mutex to avoid deadlocks
    if (state_ != State::IDLE) {
        stop();
    }

    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    double durationSeconds = 0.0;
    int64_t totalFrames = 0;

    {
        std::lock_guard<std::mutex> lock(fileMutex_);

        // Open WAV file
        if (!wavReader_.open(filePath)) {
            LOGE("Failed to open file: %s", filePath.c_str());
            return false;
        }

        numChannels = wavReader_.getNumChannels();
        if (numChannels != 84) {
            LOGE("Expected 84 channels, got %d", numChannels);
            wavReader_.close();
            return false;
        }

        sampleRate = wavReader_.getSampleRate();
        bitsPerSample = wavReader_.getBitsPerSample();
        durationSeconds = wavReader_.getDurationSeconds();
        totalFrames = wavReader_.getTotalFrames();
        sourceSampleRate_ = sampleRate;
        sourceBitsPerSample_ = bitsPerSample;
        sourceNumChannels_ = numChannels;
    }

    impulseResponseLoaded_ = loadImpulseResponse(sampleRate);
    {
        std::lock_guard<std::mutex> lock(fileMutex_);
        wavReader_.seek(0);
    }

    LOGD("=== PLAYBACK ENGINE SETUP ===");
    LOGD("File: %s", filePath.c_str());
    LOGD("Channels: %d", numChannels);
    LOGD("Sample rate: %d Hz", sampleRate);
    LOGD("Bit depth: %d", bitsPerSample);
    LOGD("Duration: %.2f seconds", durationSeconds);
    LOGD("Total frames: %lld", (long long)totalFrames);
    LOGD("Buffer size: %d frames", BUFFER_FRAMES);

    auto callback = [this](float* buffer, int32_t numFrames) {
        this->audioCallback(buffer, numFrames);
    };

    if (!audioOutput_->initialize(sampleRate, BUFFER_FRAMES, callback)) {
        LOGE("Failed to initialize audio output");
        std::lock_guard<std::mutex> lock(fileMutex_);
        wavReader_.close();
        return false;
    }

    playbackCompleted_ = false;
    state_ = State::STOPPED;

    LOGD("File loaded successfully");

    return true;
}

void PlaybackEngine::setAssetManager(AAssetManager* manager) {
    assetManager_ = manager;
    irLoader_.setAssetManager(manager);
}

void PlaybackEngine::setPreRenderCacheDirectory(const std::string& path) {
    std::lock_guard<std::mutex> lock(fileMutex_);
    preRenderCacheDir_ = path;
    clearPreRenderedState();
}

void PlaybackEngine::clearPreRenderedState() {
    preRenderedReady_ = false;
    usePreRendered_ = false;
    preRenderedFilePath_.clear();
    preRenderedSourcePath_.clear();
    preRenderProgress_.store(0, std::memory_order_relaxed);
    preRenderInProgress_.store(false, std::memory_order_relaxed);
}

bool PlaybackEngine::play() {
    if (state_ == State::IDLE || !wavReader_.isOpen()) {
        LOGE("No file loaded");
        return false;
    }

    if (!preRenderedReady_) {
        LOGW("Pre-rendered file not ready; call preparePreRenderedFile() first");
        return false;
    }

    if (state_ == State::PLAYING) {
        return true;  // Already playing
    }

    const bool completed = playbackCompleted_.exchange(false);
    {
        std::lock_guard<std::mutex> lock(fileMutex_);
        if (completed || wavReader_.getPosition() >= wavReader_.getTotalFrames()) {
            wavReader_.seek(0);
        }
    }

    if (audioOutput_->isPlaying()) {
        audioOutput_->stop();
    }

    if (audioOutput_->start()) {
        state_ = State::PLAYING;
        LOGD("Playback started");
        return true;
    }

    LOGE("Audio output failed to start");
    return false;
}

void PlaybackEngine::pause() {
    if (state_ != State::PLAYING) {
        return;
    }

    audioOutput_->pause();
    state_ = State::PAUSED;
    LOGD("Playback paused");
}

void PlaybackEngine::stop() {
    if (state_ == State::IDLE) {
        return;
    }

    audioOutput_->stop();
    
    std::lock_guard<std::mutex> lock(fileMutex_);
    wavReader_.seek(0);
    playbackCompleted_ = false;
    
    state_ = State::STOPPED;
    LOGD("Playback stopped");
}

bool PlaybackEngine::seek(double positionSeconds) {
    if (!wavReader_.isOpen()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(fileMutex_);
    
    int64_t targetFrame = (int64_t)(positionSeconds * wavReader_.getSampleRate());
    
    if (wavReader_.seek(targetFrame)) {
        LOGD("Seeked to %.2f seconds (frame %lld)", positionSeconds, (long long)targetFrame);
        return true;
    }

    return false;
}

double PlaybackEngine::getPositionSeconds() const {
    if (!wavReader_.isOpen()) {
        return 0.0;
    }

    return (double)wavReader_.getPosition() / (double)wavReader_.getSampleRate();
}

double PlaybackEngine::getDurationSeconds() const {
    return wavReader_.getDurationSeconds();
}

void PlaybackEngine::audioCallback(float* output, int32_t numFrames) {
    if (state_ != State::PLAYING) {
        // Fill with silence
        memset(output, 0, numFrames * 2 * sizeof(float));
        return;
    }

    processAudio(output, numFrames);
}

void PlaybackEngine::processAudio(float* output, int32_t numFrames) {
    std::lock_guard<std::mutex> lock(fileMutex_);

    if (!preRenderedReady_) {
        memset(output, 0, numFrames * 2 * sizeof(float));
        return;
    }

    const int32_t fileChannels = wavReader_.getNumChannels();
    int32_t framesRead = wavReader_.read(inputBuffer_.data(), numFrames);
    const float gain = playbackGainLinear_.load(std::memory_order_relaxed);

    if (framesRead <= 0) {
        memset(output, 0, numFrames * 2 * sizeof(float));

        if (state_ != State::STOPPED) {
            state_ = State::STOPPED;
        }

        if (!playbackCompleted_.exchange(true)) {
            LOGD("End of file reached");
        }
        return;
    }

    if (framesRead < numFrames) {
        std::fill(inputBuffer_.begin() + static_cast<size_t>(framesRead) * fileChannels,
                  inputBuffer_.begin() + static_cast<size_t>(numFrames) * fileChannels,
                  0.0f);
    }

    for (int32_t frame = 0; frame < numFrames; ++frame) {
        if (frame < framesRead) {
            float left = inputBuffer_[frame * fileChannels];
            float right = (fileChannels > 1) ? inputBuffer_[frame * fileChannels + 1] : left;
            left *= gain;
            right *= gain;
            output[frame * 2] = left;
            output[frame * 2 + 1] = right;
        } else {
            output[frame * 2] = 0.0f;
            output[frame * 2 + 1] = 0.0f;
        }
    }
}

bool PlaybackEngine::loadImpulseResponse(int32_t sampleRate) {
    if (!assetManager_) {
        LOGW("Asset manager not provided; skipping IR load");
        matrixConvolver_.configure(nullptr, 0);
        return false;
    }

    MatrixImpulseResponse ir;
    if (!irLoader_.loadBinaural(sampleRate, ir)) {
        LOGE("Failed to load impulse response for %d Hz", sampleRate);
        matrixConvolver_.configure(nullptr, 0);
        return false;
    }

    impulseResponse_ = std::move(ir);

    if (!impulseResponse_.isValid()) {
        LOGE("Impulse response invalid after load");
        matrixConvolver_.configure(nullptr, 0);
        return false;
    }

    constexpr int kMaxPartitions = 1;  // TEMP: keep IR within single FFT partition for performance
    const int maxIrLength = BUFFER_FRAMES * kMaxPartitions;
    const int originalIrLength = impulseResponse_.irLength;
    if (impulseResponse_.irLength > maxIrLength) {
        LOGW("Trimming IR from %d to %d samples to limit CPU load",
             impulseResponse_.irLength, maxIrLength);
        const int numChannels = impulseResponse_.numInputChannels;
        std::vector<float> trimmedLeft(static_cast<size_t>(numChannels) * maxIrLength);
        std::vector<float> trimmedRight(static_cast<size_t>(numChannels) * maxIrLength);

        for (int ch = 0; ch < numChannels; ++ch) {
            const size_t srcOffset = static_cast<size_t>(ch) * impulseResponse_.irLength;
            const size_t dstOffset = static_cast<size_t>(ch) * maxIrLength;
            std::copy_n(impulseResponse_.leftEar.begin() + srcOffset, maxIrLength,
                        trimmedLeft.begin() + dstOffset);
            std::copy_n(impulseResponse_.rightEar.begin() + srcOffset, maxIrLength,
                        trimmedRight.begin() + dstOffset);
        }

        impulseResponse_.leftEar = std::move(trimmedLeft);
        impulseResponse_.rightEar = std::move(trimmedRight);
        impulseResponse_.irLength = maxIrLength;
    }

    LOGD("Loaded IR: sampleRate=%d, irLength=%d (original %d), channels=%d",
        impulseResponse_.sampleRate, impulseResponse_.irLength, originalIrLength, impulseResponse_.numInputChannels);

    if (!matrixConvolver_.configure(&impulseResponse_, BUFFER_FRAMES)) {
        LOGE("Matrix convolver configuration failed");
        return false;
    }

    constexpr float kOutputGainDb = 12.0f;
    const float gainFactor = std::pow(10.0f, kOutputGainDb / 20.0f);
    matrixConvolver_.setOutputGain(gainFactor);
    LOGD("Matrix convolver ready (gain=%.2f)", gainFactor);

    return true;
}
bool PlaybackEngine::preparePreRenderedFile() {
    if (!impulseResponseLoaded_ || !matrixConvolver_.isReady()) {
        LOGE("Impulse response not loaded; cannot pre-render");
        return false;
    }

    audioOutput_->stop();

    std::lock_guard<std::mutex> lock(fileMutex_);

    if (sourceFilePath_.empty()) {
        LOGE("No source file set for pre-render");
        return false;
    }

    // Ensure we are working from the original multichannel file
    if (!wavReader_.isOpen() || wavReader_.getNumChannels() != sourceNumChannels_) {
        wavReader_.close();
        if (!wavReader_.open(sourceFilePath_)) {
            LOGE("Failed to reopen source file: %s", sourceFilePath_.c_str());
            return false;
        }
    }

    if (!wavReader_.seek(0)) {
        LOGE("Failed to seek source file before pre-render");
        return false;
    }

    preRenderedReady_ = false;
    usePreRendered_ = false;

    if (preRenderCacheDir_.empty()) {
        LOGE("Pre-render cache directory not configured");
        return false;
    }

    preRenderProgress_.store(0, std::memory_order_relaxed);
    preRenderInProgress_.store(true, std::memory_order_relaxed);

    const std::string tempPath = JoinPath(preRenderCacheDir_, kCacheFileName);
    LOGD("Pre-rendering source %s to %s", sourceFilePath_.c_str(), tempPath.c_str());
    std::remove(tempPath.c_str());

    WAVWriter writer;
    if (!writer.open(tempPath, sourceSampleRate_, 2, 24)) {
        LOGE("Failed to open pre-render target: %s", tempPath.c_str());
        preRenderInProgress_.store(false, std::memory_order_relaxed);
        preRenderProgress_.store(0, std::memory_order_relaxed);
        return false;
    }

    matrixConvolver_.reset();

    int64_t framesProcessed = 0;
    const int64_t totalFrames = wavReader_.getTotalFrames();
    bool ok = true;

    auto convertTo24 = [this](int32_t frames) {
        const float* src = stereoBuffer_.data();
        uint8_t* dst = stereo24Buffer_.data();

        constexpr float kScale = 8388607.0f; // 2^23 - 1
        for (int32_t i = 0; i < frames * 2; ++i) {
            float sample = std::clamp(src[i], -1.0f, 1.0f);
            int32_t value = static_cast<int32_t>(std::lrintf(sample * kScale));
            if (value > 8388607) value = 8388607;
            if (value < -8388608) value = -8388608;

            const uint32_t uvalue = static_cast<uint32_t>(value);
            const int32_t byteIndex = i * 3;
            dst[byteIndex] = static_cast<uint8_t>(uvalue & 0xFF);
            dst[byteIndex + 1] = static_cast<uint8_t>((uvalue >> 8) & 0xFF);
            dst[byteIndex + 2] = static_cast<uint8_t>((uvalue >> 16) & 0xFF);
        }
    };

    while (ok) {
        int32_t framesRead = wavReader_.read(inputBuffer_.data(), BUFFER_FRAMES);
        if (framesRead <= 0) {
            break;
        }

        if (framesRead < BUFFER_FRAMES) {
            std::fill(inputBuffer_.begin() + static_cast<size_t>(framesRead) * sourceNumChannels_,
                      inputBuffer_.begin() + static_cast<size_t>(BUFFER_FRAMES) * sourceNumChannels_,
                      0.0f);
        }

        matrixConvolver_.process(inputBuffer_.data(), stereoBuffer_.data(), BUFFER_FRAMES);

        framesProcessed += framesRead;
        if (totalFrames > 0) {
            int progress = static_cast<int>((framesProcessed * 100) / totalFrames);
            if (progress > 99) {
                progress = 99;
            }
            if (progress < 0) {
                progress = 0;
            }
            preRenderProgress_.store(progress, std::memory_order_relaxed);
        }

        const int32_t framesToWrite = framesRead;
        convertTo24(framesToWrite);

        if (!writer.writeData(stereo24Buffer_.data(),
                              static_cast<size_t>(framesToWrite) * 2 * 3)) {
            ok = false;
            break;
        }

        if (framesRead < BUFFER_FRAMES) {
            std::fill(inputBuffer_.begin(),
                      inputBuffer_.begin() + static_cast<size_t>(BUFFER_FRAMES) * sourceNumChannels_,
                      0.0f);
            matrixConvolver_.process(inputBuffer_.data(), stereoBuffer_.data(), BUFFER_FRAMES);

            convertTo24(BUFFER_FRAMES);

            if (!writer.writeData(stereo24Buffer_.data(),
                                  static_cast<size_t>(BUFFER_FRAMES) * 2 * 3)) {
                ok = false;
            }
            break;
        }
    }

    writer.close();

    if (!ok) {
        LOGE("Pre-render failed");
        wavReader_.seek(0);
        std::remove(tempPath.c_str());
        preRenderInProgress_.store(false, std::memory_order_relaxed);
        preRenderProgress_.store(0, std::memory_order_relaxed);
        return false;
    }

    wavReader_.close();

    if (!wavReader_.open(tempPath)) {
        LOGE("Failed to open pre-rendered file: %s", tempPath.c_str());
        // Attempt to restore original file to keep engine usable
        if (!wavReader_.open(sourceFilePath_)) {
            LOGE("Failed to reopen original file after pre-render failure");
        }
        preRenderInProgress_.store(false, std::memory_order_relaxed);
        preRenderProgress_.store(0, std::memory_order_relaxed);
        return false;
    }

    wavReader_.seek(0);
    preRenderedFilePath_ = tempPath;
    preRenderedReady_ = true;
    usePreRendered_ = true;
    playbackCompleted_ = false;
    state_ = State::STOPPED;
    preRenderedSourcePath_ = sourceFilePath_;

    LOGD("Pre-rendered stereo mix created: %s (processed %lld frames)",
         preRenderedFilePath_.c_str(), static_cast<long long>(framesProcessed));
    preRenderProgress_.store(100, std::memory_order_relaxed);
    preRenderInProgress_.store(false, std::memory_order_relaxed);
    return true;
}

bool PlaybackEngine::exportPreRenderedFile(const std::string& destinationPath) {
    std::lock_guard<std::mutex> lock(fileMutex_);

    if (!preRenderedReady_ || preRenderedFilePath_.empty()) {
        LOGE("No pre-rendered file available to export");
        return false;
    }

    std::ifstream src(preRenderedFilePath_, std::ios::binary);
    if (!src.is_open()) {
        LOGE("Failed to open cached file for export: %s", preRenderedFilePath_.c_str());
        return false;
    }

    std::ofstream dst(destinationPath, std::ios::binary | std::ios::trunc);
    if (!dst.is_open()) {
        LOGE("Failed to open export destination: %s", destinationPath.c_str());
        return false;
    }

    dst << src.rdbuf();
    LOGD("Exported pre-rendered file to %s", destinationPath.c_str());
    return true;
}

bool PlaybackEngine::useExistingPreRendered(const std::string& sourcePath) {
    if (preRenderCacheDir_.empty()) {
        LOGW("Cache directory not configured; cannot reuse pre-render");
        return false;
    }

    const std::string cachePath = JoinPath(preRenderCacheDir_, kCacheFileName);
    std::ifstream cacheStream(cachePath, std::ios::binary);
    if (!cacheStream.is_open()) {
        LOGW("Cached pre-render file not found at %s", cachePath.c_str());
        return false;
    }
    cacheStream.close();

    std::lock_guard<std::mutex> lock(fileMutex_);

    wavReader_.close();
    if (!wavReader_.open(cachePath)) {
        LOGE("Failed to open cached pre-render file: %s", cachePath.c_str());
        clearPreRenderedState();
        return false;
    }

    if (!wavReader_.seek(0)) {
        LOGE("Failed to seek cached pre-render file");
        wavReader_.close();
        clearPreRenderedState();
        return false;
    }

    preRenderedFilePath_ = cachePath;
    preRenderedReady_ = true;
    usePreRendered_ = true;
    playbackCompleted_ = false;
    state_ = State::STOPPED;
    preRenderedSourcePath_ = sourcePath;
    sourceSampleRate_ = wavReader_.getSampleRate();
    sourceBitsPerSample_ = wavReader_.getBitsPerSample();
    sourceNumChannels_ = wavReader_.getNumChannels();
    preRenderProgress_.store(100, std::memory_order_relaxed);
    preRenderInProgress_.store(false, std::memory_order_relaxed);

    LOGD("Reusing pre-rendered cache at %s for source %s", cachePath.c_str(), sourcePath.c_str());
    return true;
}

void PlaybackEngine::setPlaybackGainDb(float gainDb) {
    const float clampedDb = std::max(0.0f, std::min(gainDb, 24.0f));
    const float linear = std::pow(10.0f, clampedDb / 20.0f);
    playbackGainLinear_.store(linear, std::memory_order_relaxed);
}

float PlaybackEngine::getPlaybackGainDb() const {
    const float linear = playbackGainLinear_.load(std::memory_order_relaxed);
    if (linear <= 0.0f) {
        return 0.0f;
    }
    return 20.0f * std::log10(linear);
}

int32_t PlaybackEngine::getPreRenderProgress() const {
    return preRenderProgress_.load(std::memory_order_relaxed);
}

bool PlaybackEngine::isPreRenderInProgress() const {
    return preRenderInProgress_.load(std::memory_order_relaxed);
}

} // namespace spcmic

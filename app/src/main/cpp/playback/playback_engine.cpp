#include "playback_engine.h"
#include <android/log.h>
#include "wav_writer.h"
#include <android/asset_manager.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>

#include "lock_free_ring_buffer.h"

#define LOG_TAG "PlaybackEngine"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace spcmic {

namespace {

constexpr const char* kDefaultCacheFileName = "playback_cache.wav";
constexpr int kDefaultOutputChannels = 2;
constexpr size_t kRealtimeRingChunks = 6;  // Number of BUFFER_FRAMES blocks queued for realtime playback
constexpr size_t kRealtimePrimingChunks = 3;  // Minimum chunks queued before playback starts

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
    , cacheFileName_(kDefaultCacheFileName)
    , preRenderedReady_(false)
    , usePreRendered_(false)
    , sourceSampleRate_(0)
    , sourceBitsPerSample_(0)
    , sourceNumChannels_(0)
    , playbackGainLinear_(1.0f)
    , loopEnabled_(false)
    , preRenderProgress_(0)
    , preRenderInProgress_(false)
    , playbackConvolved_(false)
    , currentPreset_(IRPreset::Binaural)
    , exportOutputChannels_(kDefaultOutputChannels)
    , realtimeThreadRunning_(false)
    , realtimeThreadStopRequested_(false)
    , realtimeWorkerPrimed_(false)
{
    
    // Allocate input buffer for 84 channels
    inputBuffer_.resize(BUFFER_FRAMES * 84);
    ensureOutputBufferCapacity(exportOutputChannels_);
    
    // Create audio output
    audioOutput_ = std::make_unique<AudioOutput>();

    matrixConvolver_.reset();
}

PlaybackEngine::~PlaybackEngine() {
    stop();
}

bool PlaybackEngine::loadFile(const std::string& filePath) {
    std::lock_guard<std::mutex> loadLock(loadMutex_);
    audioOutput_->stop();
    stopRealtimeConvolutionWorker();

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

    if (playbackConvolved_.load(std::memory_order_relaxed)) {
        impulseResponseLoaded_ = loadImpulseResponse(sampleRate);
    } else {
        impulseResponseLoaded_ = false;
        matrixConvolver_.configure(nullptr, 0);
    }
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

bool PlaybackEngine::loadFileFromDescriptor(int fd, const std::string& displayPath) {
    std::lock_guard<std::mutex> loadLock(loadMutex_);
    audioOutput_->stop();
    stopRealtimeConvolutionWorker();

    clearPreRenderedState();
    sourceFilePath_ = displayPath;

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

        if (!wavReader_.openFromFd(fd, displayPath)) {
            LOGE("Failed to open descriptor for playback: %s", displayPath.c_str());
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

    if (playbackConvolved_.load(std::memory_order_relaxed)) {
        impulseResponseLoaded_ = loadImpulseResponse(sampleRate);
    } else {
        impulseResponseLoaded_ = false;
        matrixConvolver_.configure(nullptr, 0);
    }
    {
        std::lock_guard<std::mutex> lock(fileMutex_);
        wavReader_.seek(0);
    }

    LOGD("=== PLAYBACK ENGINE SETUP (FD) ===");
    LOGD("Descriptor: %s", displayPath.c_str());
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
        LOGE("Failed to initialize audio output for descriptor playback");
        std::lock_guard<std::mutex> lock(fileMutex_);
        wavReader_.close();
        return false;
    }

    playbackCompleted_ = false;
    state_ = State::STOPPED;

    LOGD("Descriptor loaded successfully");

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

void PlaybackEngine::ensureOutputBufferCapacity(int outputChannels) {
    const int channels = std::max(1, outputChannels);
    mixBuffer_.resize(static_cast<size_t>(BUFFER_FRAMES) * channels);
    mix24Buffer_.resize(static_cast<size_t>(BUFFER_FRAMES) * channels * 3);
}

void PlaybackEngine::configureExportPreset(IRPreset preset,
                                           int outputChannels,
                                           const std::string& cacheFileName) {
    const int channels = outputChannels > 0 ? outputChannels : kDefaultOutputChannels;
    const std::string resolvedCache = cacheFileName.empty() ? kDefaultCacheFileName : cacheFileName;

    {
        std::lock_guard<std::mutex> lock(fileMutex_);
        currentPreset_ = preset;
        exportOutputChannels_ = channels;
        cacheFileName_ = resolvedCache;
        impulseResponseLoaded_ = false;
        matrixConvolver_.configure(nullptr, 0);
        clearPreRenderedState();
    }

    ensureOutputBufferCapacity(channels);
}

bool PlaybackEngine::play() {
    if (state_ == State::IDLE || !wavReader_.isOpen()) {
        LOGE("No file loaded");
        return false;
    }

    const bool useConvolved = playbackConvolved_.load(std::memory_order_relaxed);
    const bool realtimeConvolution = useConvolved && !usePreRendered_;

    if (useConvolved) {
        std::lock_guard<std::mutex> lock(fileMutex_);
        if (usePreRendered_) {
            if (!preRenderedReady_) {
                LOGW("Pre-rendered file not ready; call preparePreRenderedFile() first");
                return false;
            }
        } else if (!matrixConvolver_.isReady()) {
            LOGW("Impulse response not configured; cannot start convolved playback");
            return false;
        }
    } else {
        std::lock_guard<std::mutex> lock(fileMutex_);
        if (usePreRendered_ && !sourceFilePath_.empty()) {
            LOGD("Direct playback requested; reopening original multichannel file");
            wavReader_.close();
            if (!wavReader_.open(sourceFilePath_)) {
                LOGE("Failed to reopen original source for direct playback: %s", sourceFilePath_.c_str());
                return false;
            }
            if (!wavReader_.seek(0)) {
                LOGW("Failed to seek original source after reopen");
            }
            usePreRendered_ = false;
            preRenderedReady_ = false;
        }
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

    if (realtimeConvolution && matrixConvolver_.isReady()) {
        {
            std::lock_guard<std::mutex> lock(fileMutex_);
            matrixConvolver_.reset();
        }
        startRealtimeConvolutionWorker();
        waitForRealtimePriming();
    }

    if (audioOutput_->start()) {
        state_ = State::PLAYING;
        LOGD("Playback started");
        return true;
    }

    if (realtimeConvolution) {
        stopRealtimeConvolutionWorker();
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
        stopRealtimeConvolutionWorker();
        return;
    }

    audioOutput_->stop();
    stopRealtimeConvolutionWorker();
    
    std::lock_guard<std::mutex> lock(fileMutex_);
    wavReader_.seek(0);
    playbackCompleted_ = false;

    if (playbackConvolved_.load(std::memory_order_relaxed) && !usePreRendered_ && matrixConvolver_.isReady()) {
        matrixConvolver_.reset();
    }
    
    state_ = State::STOPPED;
    LOGD("Playback stopped");
}

bool PlaybackEngine::seek(double positionSeconds) {
    if (!wavReader_.isOpen()) {
        return false;
    }

    const bool realtimeConvolution = playbackConvolved_.load(std::memory_order_relaxed) &&
                                     !usePreRendered_ &&
                                     matrixConvolver_.isReady();

    if (realtimeConvolution) {
        stopRealtimeConvolutionWorker();
    }

    bool success = false;
    {
        std::lock_guard<std::mutex> lock(fileMutex_);

        const int64_t targetFrame = static_cast<int64_t>(positionSeconds * wavReader_.getSampleRate());

        if (wavReader_.seek(targetFrame)) {
            if (realtimeConvolution) {
                matrixConvolver_.reset();
            }
            LOGD("Seeked to %.2f seconds (frame %lld)", positionSeconds, (long long)targetFrame);
            success = true;
        }
    }

    if (!success) {
        return false;
    }

    if (realtimeConvolution && state_.load(std::memory_order_relaxed) == State::PLAYING) {
        startRealtimeConvolutionWorker();
        waitForRealtimePriming();
    }

    return true;
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
    const bool useConvolved = playbackConvolved_.load(std::memory_order_relaxed);
    const bool realtimeConvolution = useConvolved && !usePreRendered_ && matrixConvolver_.isReady();

    if (realtimeConvolution) {
        if (realtimeRing_) {
            fillRealtimeOutput(output, numFrames);
        } else {
            memset(output, 0, numFrames * 2 * sizeof(float));
        }
        return;
    }

    std::lock_guard<std::mutex> lock(fileMutex_);

    if (useConvolved && usePreRendered_ && !preRenderedReady_) {
        memset(output, 0, numFrames * 2 * sizeof(float));
        return;
    }

    if (!wavReader_.isOpen()) {
        memset(output, 0, numFrames * 2 * sizeof(float));
        return;
    }

    const int32_t fileChannels = wavReader_.getNumChannels();
    int32_t framesRead = wavReader_.read(inputBuffer_.data(), numFrames);
    const float gain = playbackGainLinear_.load(std::memory_order_relaxed);
    const bool loopEnabled = loopEnabled_.load(std::memory_order_relaxed);

    if (framesRead <= 0) {
        if (loopEnabled) {
            if (wavReader_.seek(0)) {
                framesRead = wavReader_.read(inputBuffer_.data(), numFrames);
                playbackCompleted_.store(false, std::memory_order_relaxed);
            }
        }

        if (framesRead <= 0) {
            memset(output, 0, numFrames * 2 * sizeof(float));

            if (!loopEnabled) {
                if (state_ != State::STOPPED) {
                    state_ = State::STOPPED;
                }

                if (!playbackCompleted_.exchange(true)) {
                    LOGD("End of file reached");
                }
            }
            return;
        }
    }

    int32_t totalFramesRead = framesRead;

    if (loopEnabled && totalFramesRead < numFrames) {
        while (totalFramesRead < numFrames) {
            if (!wavReader_.seek(0)) {
                break;
            }

            const int32_t additional = wavReader_.read(
                inputBuffer_.data() + static_cast<size_t>(totalFramesRead) * fileChannels,
                numFrames - totalFramesRead);

            if (additional <= 0) {
                break;
            }

            totalFramesRead += additional;
        }

        framesRead = totalFramesRead;
        playbackCompleted_.store(false, std::memory_order_relaxed);
    }

    if (framesRead < numFrames) {
        std::fill(inputBuffer_.begin() + static_cast<size_t>(framesRead) * fileChannels,
                  inputBuffer_.begin() + static_cast<size_t>(numFrames) * fileChannels,
                  0.0f);

        if (!loopEnabled) {
            if (state_ != State::STOPPED) {
                state_ = State::STOPPED;
            }

            if (!playbackCompleted_.exchange(true)) {
                LOGD("End of file reached");
            }
        }
    }

    int32_t leftIndex = 0;
    int32_t rightIndex = std::min(1, std::max(fileChannels - 1, 0));

    if (!useConvolved) {
        bool channelFallback = false;
        if (DIRECT_LEFT_CHANNEL_INDEX < fileChannels) {
            leftIndex = DIRECT_LEFT_CHANNEL_INDEX;
        } else {
            channelFallback = true;
        }

        if (DIRECT_RIGHT_CHANNEL_INDEX < fileChannels) {
            rightIndex = DIRECT_RIGHT_CHANNEL_INDEX;
        } else {
            channelFallback = true;
            const int32_t lastChannel = std::max(fileChannels - 1, 0);
            rightIndex = std::min(lastChannel, leftIndex);
        }

        if (channelFallback) {
            static bool loggedFallback = false;
            if (!loggedFallback) {
                LOGW("Direct playback fallback: file has %d channels, expected > %d", fileChannels, DIRECT_RIGHT_CHANNEL_INDEX);
                loggedFallback = true;
            }
        }
    }

    for (int32_t frame = 0; frame < numFrames; ++frame) {
        if (frame < framesRead) {
            const float* frameBase = inputBuffer_.data() + static_cast<size_t>(frame) * fileChannels;
            float left = frameBase[leftIndex];
            float right = (fileChannels > rightIndex) ? frameBase[rightIndex] : frameBase[leftIndex];
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

void PlaybackEngine::startRealtimeConvolutionWorker() {
    if (!playbackConvolved_.load(std::memory_order_relaxed) || usePreRendered_) {
        return;
    }

    if (!matrixConvolver_.isReady()) {
        return;
    }

    bool expected = false;
    if (!realtimeThreadRunning_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    const int fileChannels = sourceNumChannels_;
    if (fileChannels <= 0) {
        realtimeThreadRunning_.store(false, std::memory_order_release);
        return;
    }

    const size_t chunkFrames = static_cast<size_t>(BUFFER_FRAMES);
    const int stereoChannels = 2;
    const size_t chunkBytes = chunkFrames * stereoChannels * sizeof(float);
    const size_t primingThreshold = chunkBytes * kRealtimePrimingChunks;
    const size_t capacityBytes = chunkBytes * kRealtimeRingChunks;

    try {
        std::lock_guard<std::mutex> lock(realtimeMutex_);
        if (!realtimeRing_ || realtimeRing_->getCapacity() != capacityBytes) {
            realtimeRing_ = std::make_unique<LockFreeRingBuffer>(capacityBytes);
        } else {
            realtimeRing_->reset();
        }
    } catch (...) {
        realtimeThreadRunning_.store(false, std::memory_order_release);
        throw;
    }

    realtimeWorkerPrimed_.store(false, std::memory_order_release);
    realtimeThreadStopRequested_.store(false, std::memory_order_release);

    try {
        realtimeThread_ = std::thread(&PlaybackEngine::realtimeConvolutionLoop, this);
    } catch (...) {
        realtimeThreadRunning_.store(false, std::memory_order_release);
        realtimeThreadStopRequested_.store(false, std::memory_order_release);
        throw;
    }
}

void PlaybackEngine::stopRealtimeConvolutionWorker(bool flushRing) {
    realtimeThreadStopRequested_.store(true, std::memory_order_release);
    realtimeCv_.notify_all();

    if (realtimeThread_.joinable()) {
        realtimeThread_.join();
    }

    realtimeThreadRunning_.store(false, std::memory_order_release);
    realtimeThreadStopRequested_.store(false, std::memory_order_release);
    realtimeWorkerPrimed_.store(false, std::memory_order_release);

    if (flushRing) {
        std::lock_guard<std::mutex> lock(realtimeMutex_);
        if (realtimeRing_) {
            realtimeRing_->reset();
        }
    }

    realtimeThread_ = std::thread();
}

void PlaybackEngine::realtimeConvolutionLoop() {
    const int fileChannels = sourceNumChannels_;
    if (fileChannels <= 0) {
        realtimeThreadRunning_.store(false, std::memory_order_release);
        realtimeWorkerPrimed_.store(true, std::memory_order_release);
        realtimeCv_.notify_all();
        return;
    }

    const int outChannels = std::max(1, exportOutputChannels_);
    const size_t chunkFrames = static_cast<size_t>(BUFFER_FRAMES);
    const int stereoChannels = 2;
    const size_t chunkBytes = chunkFrames * stereoChannels * sizeof(float);
    const size_t primingThreshold = chunkBytes * kRealtimePrimingChunks;

    std::vector<float> input(static_cast<size_t>(chunkFrames) * fileChannels, 0.0f);
    std::vector<float> convolved(static_cast<size_t>(chunkFrames) * outChannels, 0.0f);
    std::vector<float> stereo(static_cast<size_t>(chunkFrames) * stereoChannels, 0.0f);

    LockFreeRingBuffer* ring = nullptr;
    {
        std::lock_guard<std::mutex> lock(realtimeMutex_);
        ring = realtimeRing_.get();
    }

    if (!ring) {
        realtimeThreadRunning_.store(false, std::memory_order_release);
        realtimeWorkerPrimed_.store(true, std::memory_order_release);
        realtimeCv_.notify_all();
        return;
    }

    while (!realtimeThreadStopRequested_.load(std::memory_order_acquire)) {
        if (ring->getAvailableSpace() < chunkBytes) {
            realtimeWorkerPrimed_.store(true, std::memory_order_release);
            realtimeCv_.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        int32_t framesRead = 0;
        bool finalChunk = false;
        const bool loop = loopEnabled_.load(std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(fileMutex_);
            if (!wavReader_.isOpen()) {
                framesRead = 0;
            } else {
                framesRead = wavReader_.read(input.data(), static_cast<int32_t>(chunkFrames));
                if (framesRead <= 0 && loop) {
                    if (wavReader_.seek(0)) {
                        framesRead = wavReader_.read(input.data(), static_cast<int32_t>(chunkFrames));
                        playbackCompleted_.store(false, std::memory_order_relaxed);
                    }
                }

                if (framesRead < static_cast<int32_t>(chunkFrames) && framesRead > 0) {
                    std::fill(input.begin() + static_cast<size_t>(framesRead) * fileChannels,
                              input.end(),
                              0.0f);
                    if (!loop) {
                        finalChunk = true;
                    }
                }
            }
        }

        if (framesRead <= 0) {
            if (!loop) {
                state_ = State::STOPPED;
                if (!playbackCompleted_.exchange(true)) {
                    LOGD("Realtime worker reached end of file");
                }
                std::fill(stereo.begin(), stereo.end(), 0.0f);
                size_t written = 0;
                const uint8_t* zeroBytes = reinterpret_cast<const uint8_t*>(stereo.data());
                while (written < chunkBytes && !realtimeThreadStopRequested_.load(std::memory_order_acquire)) {
                    const size_t wrote = ring->write(zeroBytes + written, chunkBytes - written);
                    if (wrote == 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    written += wrote;
                }
                break;
            }
            continue;
        }

        matrixConvolver_.process(input.data(), convolved.data(), static_cast<int32_t>(chunkFrames));

        const float gain = playbackGainLinear_.load(std::memory_order_relaxed);
        for (size_t frame = 0; frame < chunkFrames; ++frame) {
            const size_t baseIndex = frame * outChannels;
            const float left = convolved[baseIndex];
            const float right = (outChannels > 1) ? convolved[baseIndex + 1] : left;
            stereo[frame * 2] = left * gain;
            stereo[frame * 2 + 1] = right * gain;
        }

        size_t written = 0;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(stereo.data());
        while (written < chunkBytes && !realtimeThreadStopRequested_.load(std::memory_order_acquire)) {
            const size_t wrote = ring->write(bytes + written, chunkBytes - written);
            if (wrote == 0) {
                realtimeWorkerPrimed_.store(true, std::memory_order_release);
                realtimeCv_.notify_all();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            written += wrote;
        }

        if (!realtimeWorkerPrimed_.load(std::memory_order_acquire) &&
            ring->getAvailableBytes() >= primingThreshold) {
            realtimeWorkerPrimed_.store(true, std::memory_order_release);
            realtimeCv_.notify_all();
        }

        if (finalChunk && !loop) {
            state_ = State::STOPPED;
            playbackCompleted_.store(true, std::memory_order_relaxed);
            break;
        }
    }

    realtimeThreadRunning_.store(false, std::memory_order_release);
    realtimeWorkerPrimed_.store(true, std::memory_order_release);
    realtimeCv_.notify_all();
}

void PlaybackEngine::waitForRealtimePriming() {
    if (!realtimeRing_) {
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    const size_t chunkBytes = static_cast<size_t>(BUFFER_FRAMES) * 2 * sizeof(float);
    const size_t requiredBytes = chunkBytes * kRealtimePrimingChunks;
    std::unique_lock<std::mutex> lock(realtimeMutex_);
    realtimeCv_.wait_until(lock, deadline, [this, requiredBytes]() {
        if (!realtimeThreadRunning_.load(std::memory_order_acquire)) {
            return true;
        }
        if (realtimeWorkerPrimed_.load(std::memory_order_acquire)) {
            return true;
        }
        if (realtimeRing_) {
            const size_t bytesAvailable = realtimeRing_->getAvailableBytes();
            if (bytesAvailable >= requiredBytes) {
                return true;
            }
        }
        return false;
    });

    if (realtimeRing_) {
        const size_t bytesAvailable = realtimeRing_->getAvailableBytes();
        if (bytesAvailable < requiredBytes) {
            LOGW("Realtime priming timed out with %zu bytes available (need %zu)",
                 bytesAvailable,
                 requiredBytes);
        }
    }
}

void PlaybackEngine::fillRealtimeOutput(float* output, int32_t numFrames) {
    const size_t bytesNeeded = static_cast<size_t>(numFrames) * 2 * sizeof(float);
    uint8_t* dest = reinterpret_cast<uint8_t*>(output);

    LockFreeRingBuffer* ring = realtimeRing_.get();
    if (!ring) {
        memset(output, 0, bytesNeeded);
        return;
    }

    size_t totalRead = 0;
    for (int attempt = 0; attempt < 2 && totalRead < bytesNeeded; ++attempt) {
        const size_t read = ring->read(dest + totalRead, bytesNeeded - totalRead);
        if (read == 0) {
            if (!realtimeThreadRunning_.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::yield();
        }
        totalRead += read;
    }

    if (totalRead < bytesNeeded) {
        memset(dest + totalRead, 0, bytesNeeded - totalRead);
        static std::atomic<bool> loggedUnderflow{false};
        bool expected = false;
        if (loggedUnderflow.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            LOGW("Realtime playback underflow: delivered %zu of %zu bytes", totalRead, bytesNeeded);
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
    if (!irLoader_.loadPreset(currentPreset_, sampleRate, ir)) {
        LOGE("Failed to load impulse response for preset %d at %d Hz",
             static_cast<int>(currentPreset_), sampleRate);
        matrixConvolver_.configure(nullptr, 0);
        return false;
    }

    impulseResponse_ = std::move(ir);

    if (!impulseResponse_.isValid()) {
        LOGE("Impulse response invalid after load");
        matrixConvolver_.configure(nullptr, 0);
        return false;
    }

    const int loadedOutputs = impulseResponse_.numOutputChannels;
    if (loadedOutputs <= 0) {
        LOGE("Impulse response reported zero output channels");
        matrixConvolver_.configure(nullptr, 0);
        return false;
    }

    if (loadedOutputs != exportOutputChannels_) {
        LOGW("Configured output channel count %d differs from IR (%d). Using IR value.",
             exportOutputChannels_, loadedOutputs);
        exportOutputChannels_ = loadedOutputs;
        ensureOutputBufferCapacity(exportOutputChannels_);
    }

    constexpr int kMaxPartitions = 1;  // TEMP: keep IR within single FFT partition for performance
    const int maxIrLength = BUFFER_FRAMES * kMaxPartitions;
    const int originalIrLength = impulseResponse_.irLength;
    if (impulseResponse_.irLength > maxIrLength) {
        LOGW("Trimming IR from %d to %d samples to limit CPU load",
             impulseResponse_.irLength, maxIrLength);
        const int numInputs = impulseResponse_.numInputChannels;
        const int numOutputs = impulseResponse_.numOutputChannels;
        std::vector<float> trimmed(static_cast<size_t>(numOutputs) * numInputs * maxIrLength);

        for (int outCh = 0; outCh < numOutputs; ++outCh) {
            for (int inCh = 0; inCh < numInputs; ++inCh) {
                const size_t srcOffset = (static_cast<size_t>(outCh) * numInputs +
                                          static_cast<size_t>(inCh)) * impulseResponse_.irLength;
                const size_t dstOffset = (static_cast<size_t>(outCh) * numInputs +
                                          static_cast<size_t>(inCh)) * maxIrLength;
                std::copy_n(impulseResponse_.impulseData.begin() + srcOffset,
                            maxIrLength,
                            trimmed.begin() + dstOffset);
            }
        }

        impulseResponse_.impulseData = std::move(trimmed);
        impulseResponse_.irLength = maxIrLength;
    }

    LOGD("Loaded IR: sampleRate=%d, irLength=%d (original %d), channels=%d",
        impulseResponse_.sampleRate,
        impulseResponse_.irLength,
        originalIrLength,
        impulseResponse_.numInputChannels);

    if (!matrixConvolver_.configure(&impulseResponse_, BUFFER_FRAMES)) {
        LOGE("Matrix convolver configuration failed");
        return false;
    }

    const float gainDb = (impulseResponse_.numOutputChannels == 2) ? 12.0f : 0.0f;
    const float gainFactor = std::pow(10.0f, gainDb / 20.0f);
    matrixConvolver_.setOutputGain(gainFactor);
    ensureOutputBufferCapacity(impulseResponse_.numOutputChannels);
    LOGD("Matrix convolver ready (outputs=%d, gain=%.2f)", impulseResponse_.numOutputChannels, gainFactor);

    return true;
}
bool PlaybackEngine::preparePreRenderedFile() {
    std::lock_guard<std::mutex> loadLock(loadMutex_);
    if (!impulseResponseLoaded_ || !matrixConvolver_.isReady()) {
        LOGE("Impulse response not loaded; cannot pre-render");
        return false;
    }

    audioOutput_->stop();
    stopRealtimeConvolutionWorker();

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

    const std::string tempPath = JoinPath(preRenderCacheDir_, cacheFileName_);
    LOGD("Pre-rendering source %s to %s", sourceFilePath_.c_str(), tempPath.c_str());
    std::remove(tempPath.c_str());

    const int outputChannels = std::max(1, exportOutputChannels_);
    ensureOutputBufferCapacity(outputChannels);

    WAVWriter writer;
    if (!writer.open(tempPath, sourceSampleRate_, outputChannels, 24)) {
        LOGE("Failed to open pre-render target: %s", tempPath.c_str());
        preRenderInProgress_.store(false, std::memory_order_relaxed);
        preRenderProgress_.store(0, std::memory_order_relaxed);
        return false;
    }

    matrixConvolver_.reset();

    int64_t framesProcessed = 0;
    const int64_t totalFrames = wavReader_.getTotalFrames();
    bool ok = true;

    auto convertTo24 = [this, outputChannels](int32_t frames) {
        const float* src = mixBuffer_.data();
        uint8_t* dst = mix24Buffer_.data();

        constexpr float kScale = 8388607.0f; // 2^23 - 1
        const size_t stride = static_cast<size_t>(outputChannels);
        for (int32_t frame = 0; frame < frames; ++frame) {
            for (int32_t ch = 0; ch < outputChannels; ++ch) {
                const size_t sampleIndex = static_cast<size_t>(frame) * stride + static_cast<size_t>(ch);
                float sample = std::clamp(src[sampleIndex], -1.0f, 1.0f);
                int32_t value = static_cast<int32_t>(std::lrintf(sample * kScale));
                value = std::clamp(value, -8388608, 8388607);

                const uint32_t uvalue = static_cast<uint32_t>(value);
                const size_t byteIndex = sampleIndex * 3;
                dst[byteIndex] = static_cast<uint8_t>(uvalue & 0xFF);
                dst[byteIndex + 1] = static_cast<uint8_t>((uvalue >> 8) & 0xFF);
                dst[byteIndex + 2] = static_cast<uint8_t>((uvalue >> 16) & 0xFF);
            }
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

        matrixConvolver_.process(inputBuffer_.data(), mixBuffer_.data(), BUFFER_FRAMES);

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

        if (!writer.writeData(mix24Buffer_.data(),
                              static_cast<size_t>(framesToWrite) * static_cast<size_t>(outputChannels) * 3)) {
            ok = false;
            break;
        }

        if (framesRead < BUFFER_FRAMES) {
            std::fill(inputBuffer_.begin(),
                      inputBuffer_.begin() + static_cast<size_t>(BUFFER_FRAMES) * sourceNumChannels_,
                      0.0f);
            matrixConvolver_.process(inputBuffer_.data(), mixBuffer_.data(), BUFFER_FRAMES);

            convertTo24(BUFFER_FRAMES);

            if (!writer.writeData(mix24Buffer_.data(),
                                  static_cast<size_t>(BUFFER_FRAMES) * static_cast<size_t>(outputChannels) * 3)) {
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

    LOGD("Pre-rendered mix created (%d ch): %s (processed %lld frames)",
        outputChannels,
        preRenderedFilePath_.c_str(),
        static_cast<long long>(framesProcessed));
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
    if (!playbackConvolved_.load(std::memory_order_relaxed)) {
        LOGW("Convolved playback disabled; ignoring cached pre-render request");
        return false;
    }

    if (preRenderCacheDir_.empty()) {
        LOGW("Cache directory not configured; cannot reuse pre-render");
        return false;
    }

    const std::string cachePath = JoinPath(preRenderCacheDir_, cacheFileName_);
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
    exportOutputChannels_ = sourceNumChannels_;
    ensureOutputBufferCapacity(exportOutputChannels_);
    preRenderProgress_.store(100, std::memory_order_relaxed);
    preRenderInProgress_.store(false, std::memory_order_relaxed);

    LOGD("Reusing pre-rendered cache at %s for source %s", cachePath.c_str(), sourcePath.c_str());
    return true;
}

void PlaybackEngine::setPlaybackGainDb(float gainDb) {
    const float clampedDb = std::max(0.0f, std::min(gainDb, 48.0f));
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

void PlaybackEngine::setLooping(bool enabled) {
    loopEnabled_.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        playbackCompleted_.store(false, std::memory_order_relaxed);
    }
}

bool PlaybackEngine::isLooping() const {
    return loopEnabled_.load(std::memory_order_relaxed);
}

int32_t PlaybackEngine::getPreRenderProgress() const {
    return preRenderProgress_.load(std::memory_order_relaxed);
}

bool PlaybackEngine::isPreRenderInProgress() const {
    return preRenderInProgress_.load(std::memory_order_relaxed);
}

void PlaybackEngine::setPlaybackConvolved(bool enabled) {
    const bool previous = playbackConvolved_.exchange(enabled, std::memory_order_relaxed);
    if (enabled == previous) {
        return;
    }

    stopRealtimeConvolutionWorker();

    if (enabled) {
        std::lock_guard<std::mutex> lock(fileMutex_);
        if (usePreRendered_ && !sourceFilePath_.empty()) {
            LOGD("Enabling convolved playback; restoring original multichannel source");
            wavReader_.close();
            if (wavReader_.open(sourceFilePath_)) {
                if (!wavReader_.seek(0)) {
                    LOGW("Failed to seek original source after enabling convolved playback");
                }
            } else {
                LOGE("Failed to reopen original source while enabling convolved playback: %s", sourceFilePath_.c_str());
            }
        }
        usePreRendered_ = false;
        preRenderedReady_ = false;
        if (matrixConvolver_.isReady()) {
            matrixConvolver_.reset();
        }
        return;
    }
    if (!enabled && previous) {
        std::lock_guard<std::mutex> lock(fileMutex_);
        if (usePreRendered_ && !sourceFilePath_.empty()) {
            LOGD("Disabling convolved playback; restoring original multichannel source");
            wavReader_.close();
            if (wavReader_.open(sourceFilePath_)) {
                if (!wavReader_.seek(0)) {
                    LOGW("Failed to seek original source after disabling convolved playback");
                }
            } else {
                LOGE("Failed to reopen original source while disabling convolved playback: %s", sourceFilePath_.c_str());
            }
            usePreRendered_ = false;
        }
        preRenderedReady_ = false;
    }
}

bool PlaybackEngine::isPlaybackConvolved() const {
    return playbackConvolved_.load(std::memory_order_relaxed);
}

} // namespace spcmic

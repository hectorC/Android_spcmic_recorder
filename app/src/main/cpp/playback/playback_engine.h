#ifndef SPCMIC_PLAYBACK_ENGINE_H
#define SPCMIC_PLAYBACK_ENGINE_H

#include "wav_file_reader.h"
#include "stereo_downmix.h"
#include "audio_output.h"
#include "matrix_convolver/ir_loader.h"
#include "matrix_convolver/ir_data.h"
#include "matrix_convolver/matrix_convolver.h"
struct AAssetManager;
#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

namespace spcmic {

/**
 * Main playback engine
 * Coordinates WAV file reading, downmixing, and audio output
 */
class PlaybackEngine {
public:
    enum class State {
        IDLE,
        PLAYING,
        PAUSED,
        STOPPED
    };

    PlaybackEngine();
    ~PlaybackEngine();

    /**
     * Load a 84-channel WAV file
     * @param filePath Absolute path to WAV file
     * @return true if loaded successfully
     */
    bool loadFile(const std::string& filePath);

    /**
     * Start playback
     */
    bool play();

    /**
     * Pause playback
     */
    void pause();

    /**
     * Stop playback and reset position
     */
    void stop();

    /**
     * Seek to position
     * @param positionSeconds Position in seconds
     */
    bool seek(double positionSeconds);

    /**
     * Get current playback state
     */
    State getState() const { return state_; }

    /**
     * Provide Android asset manager to access IR assets.
     */
    void setAssetManager(AAssetManager* manager);

    /**
     * Get current position in seconds
     */
    double getPositionSeconds() const;

    /**
     * Get total duration in seconds
     */
    double getDurationSeconds() const;

    /**
     * Check if file is loaded
     */
    bool isFileLoaded() const { return wavReader_.isOpen(); }

    /**
     * Configure directory used for pre-render cache
     */
    void setPreRenderCacheDirectory(const std::string& path);

    /**
     * Pre-render the currently loaded file to stereo cache
     */
    bool preparePreRenderedFile();

    /**
     * Check if pre-rendered stereo cache is ready
     */
    bool isPreRenderedReady() const { return preRenderedReady_; }

    /**
     * Access path for pre-rendered stereo cache
     */
    const std::string& getPreRenderedFilePath() const { return preRenderedFilePath_; }

    /**
     * Export cached stereo file to provided destination path
     */
    bool exportPreRenderedFile(const std::string& destinationPath);

    /**
     * Attempt to reuse an existing cached pre-render for the given source
     */
    bool useExistingPreRendered(const std::string& sourcePath);

    /**
     * Adjust playback gain in decibels (0 - 48 dB)
     */
    void setPlaybackGainDb(float gainDb);

    /**
     * Get the current playback gain in decibels
     */
    float getPlaybackGainDb() const;

    /**
     * Enable or disable looping playback
     */
    void setLooping(bool enabled);

    /**
     * Query whether looping playback is enabled
     */
    bool isLooping() const;

    /**
     * Query progress of the current pre-render operation (0-100)
     */
    int32_t getPreRenderProgress() const;

    /**
     * Check if a pre-render operation is active
     */
    bool isPreRenderInProgress() const;

private:
    /**
     * Audio callback - fills output buffer
     */
    void audioCallback(float* output, int32_t numFrames);

    /**
     * Read and process audio
     */
    void processAudio(float* output, int32_t numFrames);

    bool loadImpulseResponse(int32_t sampleRate);
    void clearPreRenderedState();

    WavFileReader wavReader_;
    StereoDownmix downmix_;
    std::unique_ptr<AudioOutput> audioOutput_;
    IRLoader irLoader_;
    MatrixImpulseResponse impulseResponse_;
    MatrixConvolver matrixConvolver_;
    bool impulseResponseLoaded_;

    std::atomic<State> state_;
    std::atomic<bool> playbackCompleted_;
    std::mutex fileMutex_;

    // Processing buffers
    std::vector<float> inputBuffer_;    // Multichannel buffer (max 84 channels)
    std::vector<float> stereoBuffer_;   // Stereo processing buffer
    std::vector<uint8_t> stereo24Buffer_; // Stereo 24-bit buffer for file writes

    AAssetManager* assetManager_;
    std::string sourceFilePath_;
    std::string preRenderedFilePath_;
    std::string preRenderCacheDir_;
    std::string preRenderedSourcePath_;
    bool preRenderedReady_;
    bool usePreRendered_;
    int32_t sourceSampleRate_;
    int32_t sourceBitsPerSample_;
    int32_t sourceNumChannels_;
    std::atomic<float> playbackGainLinear_;
    std::atomic<bool> loopEnabled_;
    std::atomic<int32_t> preRenderProgress_;
    std::atomic<bool> preRenderInProgress_;
    
    static constexpr int32_t BUFFER_FRAMES = 4096;  // ~85ms at 48kHz to improve stability
};

} // namespace spcmic

#endif // SPCMIC_PLAYBACK_ENGINE_H

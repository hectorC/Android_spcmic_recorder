#pragma once

#include <vector>
#include <cstdint>

class USBAudioInterface {
public:
    USBAudioInterface();
    ~USBAudioInterface();
    
    bool initialize(int deviceFd, int sampleRate, int channelCount);
    bool startStreaming();
    bool stopStreaming();
    void release();
    
    // Read audio data from USB device
    size_t readAudioData(uint8_t* buffer, size_t bufferSize);

    // Buffer sizing helpers
    size_t getRecommendedBufferSize() const;
    size_t getUrbBufferSize() const { return m_urbBufferSize; }
    size_t getIsoPacketSize() const { return m_isoPacketSize; }
    double getEffectiveSampleRate() const { return m_effectiveSampleRate; }
    int getEffectiveSampleRateRounded() const;
    bool setTargetSampleRate(int sampleRate);
    const std::vector<uint32_t>& getSupportedSampleRates() const { return m_supportedSampleRates; }
    bool supportsContinuousSampleRate() const { return m_supportsContinuousSampleRate; }
    uint32_t getContinuousSampleRateMin() const { return m_minContinuousSampleRate; }
    uint32_t getContinuousSampleRateMax() const { return m_maxContinuousSampleRate; }
    
    // Get device capabilities
    int getSampleRate() const { return m_sampleRate; }
    int getChannelCount() const { return m_channelCount; }
    int getBytesPerSample() const { return m_bytesPerSample; }
    
    // USB Audio Class specific
    bool enableAudioStreaming();
    bool setInterface(int interfaceNum, int altSetting);
    
private:
    int m_deviceFd;
    int m_sampleRate;
    int m_channelCount;
    int m_bytesPerSample;
    bool m_isStreaming;

    // USB endpoints
    int m_audioInEndpoint;
    int m_controlEndpoint;

    // URB management counters (member variables to avoid static variable memory corruption)
    int m_nextSubmitIndex;
    int m_totalSubmitted;
    int m_callCount;
    int m_attemptCount;
    int m_submitErrorCount;
    int m_reapCount;
    int m_reapErrorCount;
    int m_eagainCount;
    int m_reapAttemptCount;

    // Stuck URB detection - track when only one URB keeps completing
    void* m_lastReapedUrbAddress;
    int m_consecutiveSameUrbCount;
    int m_recentReapCheckpoint;  // Track reap count at last check
    bool m_stuckUrbDetected;
    static const int STUCK_URB_THRESHOLD = 50; // Consider URB stuck after 50 consecutive reaps
    static const int CHECK_INTERVAL = 100; // Check for stuck URBs every 100 reaps
    static const int NUM_URBS = 64;  // Increased from 32 for better buffering against USB scheduling jitter

    // URB management - converted from static to prevent memory corruption
    struct usbdevfs_urb** m_urbs;
    uint8_t** m_urbBuffers;  // Dynamic allocation instead of static array
    bool m_urbsInitialized;
    bool m_wasStreaming;
    int m_notStreamingCount;
    int m_noFramesCount;
    std::vector<uint8_t> m_pendingData;
    size_t m_pendingReadOffset;

    // Explicit frame scheduling for isochronous transfers
    int m_currentFrameNumber;
    bool m_frameNumberInitialized;

    // USB control functions
    bool configureSampleRate(int sampleRate);
    bool findAudioEndpoint();
    bool fetchConfigurationDescriptor(std::vector<uint8_t>& descriptor);
    bool parseStreamingEndpoint(const std::vector<uint8_t>& descriptor);
    void releaseUrbResources();
    bool ensureUrbResources();
    void updateEffectiveSampleRate();
    bool readSampleRateFromClock(uint32_t& outRate);
    bool readSampleRateFromEndpoint(uint32_t& outRate);
    bool queryCurrentSampleRate(uint32_t& outRate, const char** sourceName);
    void resetStreamingState();
    bool flushIsochronousEndpoint();

    // Streaming endpoint details
    int m_streamInterfaceNumber;
    int m_streamAltSetting;
    size_t m_isoPacketSize;
    size_t m_packetsPerUrb;
    size_t m_urbBufferSize;
    size_t m_bytesPerInterval;
    size_t m_packetsPerServiceInterval;
    bool m_endpointInfoReady;
    bool m_isHighSpeed;
    bool m_isSuperSpeed;
    double m_effectiveSampleRate;
    int m_controlInterfaceNumber;
    int m_clockSourceId;
    bool m_clockFrequencyProgrammable;
    int m_streamClockEntityId;
    int m_clockSelectorId;
    std::vector<uint8_t> m_clockSelectorInputs;
    uint8_t m_clockSelectorControls;
    int m_clockMultiplierId;
    uint8_t m_clockMultiplierControls;
    struct ClockSourceDetails {
        uint8_t id;
        uint8_t attributes;
        uint8_t controls;
        bool programmable;
    };
    std::vector<ClockSourceDetails> m_clockSources;
    std::vector<uint32_t> m_supportedSampleRates;
    bool m_supportsContinuousSampleRate;
    uint32_t m_minContinuousSampleRate;
    uint32_t m_maxContinuousSampleRate;

    static constexpr size_t MAX_URB_BUFFER_BYTES = 128 * 1024;  // Increased from 64KB for lower overhead
    static constexpr size_t MAX_PENDING_BUFFER_BYTES = 512 * 1024;  // Diagnostic threshold for staged packet spillover
};
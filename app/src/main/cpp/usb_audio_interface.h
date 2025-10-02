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
    
    // Get device capabilities
    int getSampleRate() const { return m_sampleRate; }
    int getChannelCount() const { return m_channelCount; }
    int getBytesPerSample() const { return m_bytesPerSample; }
    
    // USB Audio Class specific
    bool configureUACDevice();
    bool setAudioFormat();
    bool enableAudioStreaming();
    
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
    static const int NUM_URBS = 32;

    // URB management - converted from static to prevent memory corruption
    struct usbdevfs_urb** m_urbs;
    uint8_t** m_urbBuffers;  // Dynamic allocation instead of static array
    bool m_urbsInitialized;
    bool m_wasStreaming;
    int m_notStreamingCount;
    int m_noFramesCount;

    // Explicit frame scheduling for isochronous transfers
    int m_currentFrameNumber;
    bool m_frameNumberInitialized;

    // USB control functions
    bool sendControlRequest(uint8_t request, uint16_t value, uint16_t index, 
                           uint8_t* data, uint16_t length);
    bool setInterface(int interfaceNum, int altSetting);
    bool configureSampleRate(int sampleRate);
    bool configureChannels(int channels);
    bool findAudioEndpoint();
    bool fetchConfigurationDescriptor(std::vector<uint8_t>& descriptor);
    bool parseStreamingEndpoint(const std::vector<uint8_t>& descriptor);
    void releaseUrbResources();
    bool ensureUrbResources();

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

    static constexpr size_t MAX_URB_BUFFER_BYTES = 64 * 1024;
};
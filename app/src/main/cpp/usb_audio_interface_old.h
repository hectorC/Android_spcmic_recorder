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
    
    // USB control functions
    bool sendControlRequest(uint8_t request, uint16_t value, uint16_t index, 
                           uint8_t* data, uint16_t length);
    bool setInterface(int interfaceNum, int altSetting);
    bool configureSampleRate(int sampleRate);
    bool configureChannels(int channels);
    bool findAudioEndpoint();
    
    // Buffer management
    static const size_t BUFFER_SIZE = 8192;
    uint8_t m_buffer[BUFFER_SIZE];
};
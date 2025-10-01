#pragma once

#include <cstdint>

// Forward declare tinyalsa structures
struct pcm;

class USBAudioInterface {
public:
    USBAudioInterface();
    ~USBAudioInterface();
    
    bool initialize(int deviceFd, int sampleRate, int channelCount);
    bool startStreaming();
    bool stopStreaming();
    void release();
    
    // Read audio data from USB Audio Class device via ALSA
    size_t readAudioData(uint8_t* buffer, size_t bufferSize);
    
    // Get device capabilities
    int getSampleRate() const { return m_sampleRate; }
    int getChannelCount() const { return m_channelCount; }
    int getBytesPerSample() const { return m_bytesPerSample; }
    
private:
    int m_deviceFd;  // USB device FD (kept for device detection)
    int m_sampleRate;
    int m_channelCount;
    int m_bytesPerSample;
    bool m_isStreaming;
    
    // ALSA PCM device for USB Audio Class
    struct pcm* m_pcmDevice;
    unsigned int m_alsaCard;
    unsigned int m_alsaDevice;
    
    // USB Audio Class via tinyalsa
    bool findUSBAudioCard();
    bool openAlsaDevice();
    void closeAlsaDevice();
};

#pragma once

#include <cstdint>

// Simple ALSA PCM wrapper for direct /dev/snd access
class AlsaPcm {
public:
    AlsaPcm();
    ~AlsaPcm();
    
    bool open(unsigned int card, unsigned int device, unsigned int channels, 
              unsigned int rate, unsigned int periodSize);
    void close();
    
    int read(void* buffer, unsigned int bytes);
    bool isOpen() const { return m_fd >= 0; }
    
private:
    int m_fd;
    unsigned int m_channels;
    unsigned int m_rate;
    unsigned int m_periodSize;
};

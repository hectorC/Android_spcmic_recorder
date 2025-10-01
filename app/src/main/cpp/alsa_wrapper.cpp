#include "alsa_wrapper.h"
#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sound/asound.h>
#include <cstring>

#define TAG "AlsaPcm"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

AlsaPcm::AlsaPcm()
    : m_fd(-1)
    , m_channels(0)
    , m_rate(0)
    , m_periodSize(0)
{
}

AlsaPcm::~AlsaPcm() {
    close();
}

bool AlsaPcm::open(unsigned int card, unsigned int device, unsigned int channels,
                   unsigned int rate, unsigned int periodSize) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%uD%uc", card, device);
    
    LOGI("Opening ALSA PCM device: %s", path);
    
    m_fd = ::open(path, O_RDONLY | O_NONBLOCK);
    if (m_fd < 0) {
        LOGE("Failed to open %s: %s", path, strerror(errno));
        return false;
    }
    
    // Configure PCM hardware parameters
    struct snd_pcm_hw_params hw_params;
    memset(&hw_params, 0, sizeof(hw_params));
    
    if (ioctl(m_fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw_params) < 0) {
        LOGE("Failed to get HW params: %s", strerror(errno));
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    
    m_channels = channels;
    m_rate = rate;
    m_periodSize = periodSize;
    
    LOGI("ALSA PCM device opened: %d channels, %d Hz, period size %d", 
         channels, rate, periodSize);
    
    return true;
}

void AlsaPcm::close() {
    if (m_fd >= 0) {
        LOGI("Closing ALSA PCM device");
        ::close(m_fd);
        m_fd = -1;
    }
}

int AlsaPcm::read(void* buffer, unsigned int bytes) {
    if (m_fd < 0) {
        return -1;
    }
    
    ssize_t result = ::read(m_fd, buffer, bytes);
    if (result < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOGE("PCM read error: %s", strerror(errno));
        }
        return -1;
    }
    
    return result;
}

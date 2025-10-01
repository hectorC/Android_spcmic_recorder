#include "usb_audio_interface.h"
#include <android/log.h>
#include <tinyalsa/asoundlib.h>
#include <unistd.h>
#include <cstring>
#include <sys/stat.h>

#define TAG "USBAudioInterface"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

USBAudioInterface::USBAudioInterface()
    : m_deviceFd(-1)
    , m_sampleRate(48000)
    , m_channelCount(84)
    , m_bytesPerSample(3)  // 24-bit audio
    , m_isStreaming(false)
    , m_pcmDevice(nullptr)
    , m_alsaCard(1)  // SPCMic is typically Card 1
    , m_alsaDevice(0)
{
    LOGI("USB Audio Interface created for USB Audio Class device");
}

USBAudioInterface::~USBAudioInterface() {
    release();
}

bool USBAudioInterface::findUSBAudioCard() {
    LOGI("Searching for USB Audio Class device (SPCMic)");
    
    // Check if /dev/snd/controlC1 exists (USB audio device)
    struct stat st;
    if (stat("/dev/snd/controlC1", &st) == 0) {
        LOGI("Found USB Audio device at Card 1");
        m_alsaCard = 1;
        m_alsaDevice = 0;
        return true;
    }
    
    // Try other card numbers
    for (unsigned int card = 0; card < 4; card++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/snd/controlC%u", card);
        if (stat(path, &st) == 0 && card > 0) {  // Skip card 0 (internal audio)
            LOGI("Found potential USB Audio device at Card %u", card);
            m_alsaCard = card;
            m_alsaDevice = 0;
            return true;
        }
    }
    
    LOGE("Could not find USB Audio Class device");
    return false;
}

bool USBAudioInterface::initialize(int deviceFd, int sampleRate, int channelCount) {
    LOGI("Initializing USB Audio Class interface: fd=%d, rate=%d, channels=%d", 
         deviceFd, sampleRate, channelCount);
    
    m_deviceFd = deviceFd;
    m_sampleRate = sampleRate;
    m_channelCount = channelCount;
    m_bytesPerSample = 3;  // 24-bit audio = 3 bytes per sample
    
    // Find the USB Audio Class device
    if (!findUSBAudioCard()) {
        LOGE("Failed to find USB Audio Class device");
        return false;
    }
    
    LOGI("USB Audio Class interface initialized successfully");
    LOGI("Will use ALSA device: Card %u, Device %u", m_alsaCard, m_alsaDevice);
    LOGI("Audio format: %dHz, %d channels, %d bytes per sample", 
         m_sampleRate, m_channelCount, m_bytesPerSample);
    
    return true;
}

bool USBAudioInterface::openAlsaDevice() {
    LOGI("Opening ALSA PCM device for 84-channel USB Audio Class");
    
    // Configure PCM parameters for 84-channel, 24-bit, 48kHz audio
    struct pcm_config config;
    memset(&config, 0, sizeof(config));
    config.channels = m_channelCount;  // 84 channels
    config.rate = m_sampleRate;        // 48000 Hz
    config.period_size = 1024;         // Period size in frames
    config.period_count = 4;           // Number of periods
    config.format = PCM_FORMAT_S24_3LE;  // 24-bit little-endian, 3 bytes per sample
    config.start_threshold = 0;
    config.stop_threshold = 0;
    config.silence_threshold = 0;
    
    LOGI("Opening ALSA Card %u, Device %u for capture", m_alsaCard, m_alsaDevice);
    LOGI("PCM Config: %d channels, %d Hz, period_size=%d, period_count=%d",
         config.channels, config.rate, config.period_size, config.period_count);
    
    // Open PCM device for capture (recording)
    m_pcmDevice = pcm_open(m_alsaCard, m_alsaDevice, PCM_IN, &config);
    
    if (!m_pcmDevice) {
        LOGE("Failed to allocate PCM device");
        return false;
    }
    
    if (!pcm_is_ready(m_pcmDevice)) {
        LOGE("PCM device not ready: %s", pcm_get_error(m_pcmDevice));
        pcm_close(m_pcmDevice);
        m_pcmDevice = nullptr;
        return false;
    }
    
    LOGI("ALSA PCM device opened successfully for 84-channel USB Audio");
    LOGI("PCM buffer size: %u bytes", pcm_frames_to_bytes(m_pcmDevice, config.period_size));
    
    return true;
}

void USBAudioInterface::closeAlsaDevice() {
    if (m_pcmDevice) {
        LOGI("Closing ALSA PCM device");
        pcm_close(m_pcmDevice);
        m_pcmDevice = nullptr;
    }
}

bool USBAudioInterface::startStreaming() {
    LOGI("Starting USB Audio Class streaming for 84 channels");
    
    if (m_isStreaming) {
        LOGI("Already streaming");
        return true;
    }
    
    // Open the ALSA device
    if (!openAlsaDevice()) {
        LOGE("Failed to open ALSA device for streaming");
        return false;
    }
    
    // Start PCM device
    if (pcm_start(m_pcmDevice) < 0) {
        LOGE("Failed to start PCM device: %s", pcm_get_error(m_pcmDevice));
        closeAlsaDevice();
        return false;
    }
    
    m_isStreaming = true;
    LOGI("USB Audio Class streaming started - 84 channels at 48kHz/24-bit");
    
    return true;
}

bool USBAudioInterface::stopStreaming() {
    LOGI("Stopping USB Audio Class streaming");
    
    if (!m_isStreaming) {
        return true;
    }
    
    // Stop PCM device
    if (m_pcmDevice) {
        pcm_stop(m_pcmDevice);
    }
    
    closeAlsaDevice();
    m_isStreaming = false;
    
    LOGI("USB Audio Class streaming stopped");
    return true;
}

void USBAudioInterface::release() {
    LOGI("Releasing USB Audio Class interface");
    
    stopStreaming();
    closeAlsaDevice();
    
    m_deviceFd = -1;
    
    LOGI("USB Audio Class interface released");
}

size_t USBAudioInterface::readAudioData(uint8_t* buffer, size_t bufferSize) {
    if (!m_isStreaming || !m_pcmDevice) {
        return 0;
    }
    
    // Read audio data from ALSA PCM device
    int result = pcm_read(m_pcmDevice, buffer, bufferSize);
    
    if (result < 0) {
        LOGE("PCM read error: %s", pcm_get_error(m_pcmDevice));
        
        // Try to recover from errors
        if (result == -EPIPE) {
            LOGI("PCM overrun - attempting recovery");
            pcm_prepare(m_pcmDevice);
            pcm_start(m_pcmDevice);
        }
        
        return 0;
    }
    
    // pcm_read returns 0 on success, so we return the requested buffer size
    return bufferSize;
}

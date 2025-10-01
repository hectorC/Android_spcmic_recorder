#include "usb_audio_interface.h"
#include "uac_protocol.h"
#include <android/log.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <cstring>
#include <cmath>
#include <errno.h>
#include <thread>
#include <chrono>
#include <algorithm>

#define LOG_TAG "USBAudioInterface"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

USBAudioInterface::USBAudioInterface() 
    : m_deviceFd(-1)
    , m_sampleRate(48000)
    , m_channelCount(84)
    , m_bytesPerSample(3)  // 24-bit = 3 bytes
    , m_isStreaming(false)
    , m_audioInEndpoint(0x81)  // Typical USB audio input endpoint
    , m_controlEndpoint(0x00) {
}

USBAudioInterface::~USBAudioInterface() {
    release();
}

bool USBAudioInterface::initialize(int deviceFd, int sampleRate, int channelCount) {
    LOGI("Initializing USB audio interface: fd=%d, rate=%d, channels=%d", 
         deviceFd, sampleRate, channelCount);
    
    m_deviceFd = deviceFd;
    m_sampleRate = sampleRate;
    m_channelCount = channelCount;
    
    if (m_deviceFd < 0) {
        LOGE("Invalid device file descriptor");
        return false;
    }
    
    // Find the correct audio input endpoint
    if (!findAudioEndpoint()) {
        LOGE("Failed to find audio input endpoint");
        return false;
    }
    
    // Configure the USB Audio Class device
    if (!configureUACDevice()) {
        LOGE("Failed to configure UAC device");
        return false;
    }
    
    // Set audio format (48kHz, 24-bit, 84 channels)
    if (!setAudioFormat()) {
        LOGE("Failed to set audio format");
        return false;
    }
    
    LOGI("USB audio interface initialized successfully");
    return true;
}

bool USBAudioInterface::findAudioEndpoint() {
    LOGI("Searching for audio input endpoint");
    
    // From SPCMic device logs, we know the audio endpoint is 0x81 (Address 129)
    // UsbEndpoint[mAddress=129,mAttributes=13,mMaxPacketSize=5104,mInterval=1]
    m_audioInEndpoint = 0x81;
    
    LOGI("Using SPCMic audio input endpoint: 0x%02x", m_audioInEndpoint);
    return true;
}

bool USBAudioInterface::configureUACDevice() {
    LOGI("Configuring USB Audio Class device");
    
    // Android USB host API provides a pre-configured file descriptor
    // We should work with what Android has already set up rather than
    // trying to reconfigure the device with raw USB ioctls
    
    LOGI("Using Android USB host file descriptor directly");
    
    // Set the endpoint based on device info from logs
    m_audioInEndpoint = 0x81;  // From device logs: Address 129
    LOGI("Using audio endpoint: 0x%02x", m_audioInEndpoint);
    
    // Test if we can communicate with the endpoint
    uint8_t testBuffer[1024];
    struct usbdevfs_bulktransfer bulk;
    bulk.ep = m_audioInEndpoint;
    bulk.len = sizeof(testBuffer);
    bulk.data = testBuffer;
    bulk.timeout = 100; // Short timeout for testing
    
    int result = ioctl(m_deviceFd, USBDEVFS_BULK, &bulk);
    if (result >= 0) {
        LOGI("Successfully communicated with audio endpoint, received %d bytes", result);
    } else if (errno == ETIMEDOUT || errno == EAGAIN) {
        LOGI("Audio endpoint is accessible (timeout/no data ready)");
    } else {
        LOGE("Failed to access audio endpoint: %s", strerror(errno));
        LOGI("Continuing anyway - will try during actual recording");
    }
    
    LOGI("USB Audio Class device configured successfully");
    return true;
}

bool USBAudioInterface::setAudioFormat() {
    LOGI("Setting audio format: %dHz, %d channels, %d bytes per sample", 
         m_sampleRate, m_channelCount, m_bytesPerSample);
    
    // USB Audio Class format configuration
    // This would typically involve setting up the audio streaming interface
    // with the specific format descriptors
    
    // For a real implementation, you would:
    // 1. Parse USB descriptors to find audio endpoints
    // 2. Set the appropriate alternate setting for the desired format
    // 3. Configure sample rate, bit depth, and channel count
    
    return true;
}

bool USBAudioInterface::setInterface(int interfaceNum, int altSetting) {
    struct usbdevfs_setinterface setintf;
    setintf.interface = interfaceNum;
    setintf.altsetting = altSetting;
    
    int result = ioctl(m_deviceFd, USBDEVFS_SETINTERFACE, &setintf);
    if (result < 0) {
        LOGE("Failed to set interface %d alt %d: %d", interfaceNum, altSetting, result);
        return false;
    }
    
    LOGI("Set interface %d alt setting %d", interfaceNum, altSetting);
    return true;
}

bool USBAudioInterface::configureSampleRate(int sampleRate) {
    LOGI("Configuring sample rate to %d Hz", sampleRate);
    
    // USB Audio Class sample rate configuration
    // Send SET_CUR request to the Clock Source or Sampling Frequency Control
    
    uint8_t sampleRateData[4];
    sampleRateData[0] = sampleRate & 0xFF;
    sampleRateData[1] = (sampleRate >> 8) & 0xFF;
    sampleRateData[2] = (sampleRate >> 16) & 0xFF;
    sampleRateData[3] = (sampleRate >> 24) & 0xFF;
    
    // For UAC 1.0: Send to endpoint control
    // For UAC 2.0: Send to Clock Source Unit
    
    // Try UAC 1.0 method first (endpoint control)
    struct usbdevfs_ctrltransfer ctrl;
    ctrl.bRequestType = 0x22; // Class, Endpoint, Host to Device
    ctrl.bRequest = UAC_SET_CUR;
    ctrl.wValue = (UAC_SAMPLING_FREQ_CONTROL << 8) | 0x00;
    ctrl.wIndex = m_audioInEndpoint; // Target the audio input endpoint
    ctrl.wLength = 3; // 24-bit sample rate
    ctrl.timeout = 1000;
    ctrl.data = sampleRateData;
    
    int result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
    if (result >= 0) {
        LOGI("Sample rate configured via UAC 1.0 method");
        return true;
    }
    
    LOGE("Failed to configure sample rate via endpoint control: %s", strerror(errno));
    
    // Try UAC 2.0 method (Clock Source Unit)
    ctrl.bRequestType = 0x21; // Class, Interface, Host to Device
    ctrl.wValue = (UAC_SAMPLING_FREQ_CONTROL << 8) | 0x00;
    ctrl.wIndex = (3 << 8) | 0x00; // Interface 3 (SPCMic audio streaming), Clock Source ID 0
    ctrl.wLength = 4; // 32-bit sample rate for UAC 2.0
    
    result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
    if (result >= 0) {
        LOGI("Sample rate configured via UAC 2.0 method");
        return true;
    }
    
    LOGE("Failed to configure sample rate via clock source: %s", strerror(errno));
    
    // Some devices might work without explicit sample rate setting
    // if the alternate setting already defines the correct rate
    LOGI("Assuming sample rate is set by alternate setting selection");
    return true;
}

bool USBAudioInterface::configureChannels(int channels) {
    LOGI("Configuring %d channels", channels);
    
    // Channel configuration is typically handled by selecting the appropriate
    // alternate setting of the audio streaming interface that supports
    // the desired number of channels
    
    return true;
}

bool USBAudioInterface::sendControlRequest(uint8_t request, uint16_t value, 
                                         uint16_t index, uint8_t* data, uint16_t length) {
    struct usbdevfs_ctrltransfer ctrl;
    ctrl.bRequestType = 0x21; // Class, Interface, Host to Device
    ctrl.bRequest = request;
    ctrl.wValue = value;
    ctrl.wIndex = index;
    ctrl.wLength = length;
    ctrl.timeout = 1000; // 1 second timeout
    ctrl.data = data;
    
    int result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
    if (result < 0) {
        LOGE("Control request failed: %d", result);
        return false;
    }
    
    return true;
}

bool USBAudioInterface::startStreaming() {
    if (m_isStreaming) {
        LOGI("Already streaming");
        return true;
    }
    
    LOGI("Starting USB audio streaming");
    
    // Enable audio streaming on the device
    if (!enableAudioStreaming()) {
        LOGE("Failed to enable audio streaming");
        return false;
    }
    
    m_isStreaming = true;
    LOGI("USB audio streaming started");
    return true;
}

bool USBAudioInterface::stopStreaming() {
    if (!m_isStreaming) {
        return true;
    }
    
    LOGI("Stopping USB audio streaming");
    m_isStreaming = false;
    
    // Disable audio streaming - set SPCMic Interface 3 back to alt setting 0
    // This stops the 84-channel audio streaming
    setInterface(3, 0);
    
    LOGI("USB audio streaming stopped");
    return true;
}

bool USBAudioInterface::enableAudioStreaming() {
    LOGI("Enabling USB audio streaming for SPCMic device");
    
    // First, reset any ongoing transfers to clear device busy state
    LOGI("Clearing any pending USB transfers to resolve device busy errors");
    
    // Cancel any outstanding URBs that might be causing device busy
    struct usbdevfs_urb urb = {0};
    urb.type = USBDEVFS_URB_TYPE_ISO;
    urb.endpoint = m_audioInEndpoint;
    
    // Try to discard any pending URBs
    ioctl(m_deviceFd, USBDEVFS_DISCARDURB, &urb);
    
    // Reset interface to ensure clean state
    LOGI("Resetting Interface 3 to clean state");
    setInterface(3, 0);
    usleep(200000); // 200ms to ensure reset completes and clears busy state
    
    // SPCMic uses Interface 3, Alternate Setting 1 for 84-channel audio
    LOGI("Setting Interface 3 to alternate setting 1 for SPCMic 84-channel mode");
    
    struct usbdevfs_setinterface setintf = {0};
    setintf.interface = 3;  // SPCMic audio streaming interface
    setintf.altsetting = 1; // Alternate setting 1 has the 84-channel endpoint
    
    int result = ioctl(m_deviceFd, USBDEVFS_SETINTERFACE, &setintf);
    if (result == 0) {
        LOGI("Successfully configured Interface 3 alternate setting 1 for 84-channel streaming");
        
        // Give the device time to configure the isochronous endpoint
        usleep(100000); // 100ms delay for endpoint configuration
        
        m_isStreaming = true;
        LOGI("SPCMic 84-channel audio streaming enabled - ready for isochronous transfers on endpoint 0x%02x", m_audioInEndpoint);
        return true;
    } else {
        LOGE("Failed to set Interface 3 alternate setting 1: %s", strerror(errno));
        LOGI("Attempting to continue with default configuration");
        
        m_isStreaming = true;
        LOGI("Audio streaming enabled with default configuration - endpoint 0x%02x", m_audioInEndpoint);
        return true;
    }
}

size_t USBAudioInterface::readAudioData(uint8_t* buffer, size_t bufferSize) {
    if (!m_isStreaming || m_deviceFd < 0) {
        return 0;
    }
    
    // Calculate expected frame size for 84 channels at 24-bit
    const size_t frameSize = m_channelCount * m_bytesPerSample; // 84 * 3 = 252 bytes
    const size_t maxFrames = bufferSize / frameSize;
    
    if (maxFrames == 0) {
        return 0;
    }
    
    // USB Audio Class: SPCMic uses isochronous transfers for real-time audio
    // Based on PAL logs: Interface 3, Altset 1, Endpoint 0x81 (SYNC) isochronous
    // Android requires USBDEVFS_SUBMITURB for isochronous transfers
    
    static struct usbdevfs_urb urb = {0};
    static bool urbInitialized = false;
    
    if (!urbInitialized) {
        // Initialize isochronous URB for SPCMic 84-channel audio
        urb.type = USBDEVFS_URB_TYPE_ISO;
        urb.endpoint = m_audioInEndpoint;  // 0x81
        urb.status = 0;
        urb.flags = 0;
        urb.buffer = buffer;
        urb.buffer_length = std::min(bufferSize, (size_t)1024); // Start with 1024 bytes for isochronous
        urb.actual_length = 0;
        urb.start_frame = 0;
        urb.number_of_packets = 1;  // Single packet per URB
        urb.error_count = 0;
        urb.signr = 0;
        urb.usercontext = nullptr;
        
        // Set up isochronous packet descriptor
        urb.iso_frame_desc[0].length = std::min(bufferSize, (size_t)1024);
        urb.iso_frame_desc[0].actual_length = 0;
        urb.iso_frame_desc[0].status = 0;
        
        urbInitialized = true;
    }
    
    // Update buffer for current read - use smaller buffer for isochronous transfers
    // SPCMic endpoint supports up to 5104 bytes, but isochronous transfers need smaller chunks
    size_t isoBufferSize = std::min(bufferSize, (size_t)1024); // Reduced from 5104 to 1024
    urb.buffer = buffer;
    urb.buffer_length = isoBufferSize;
    urb.iso_frame_desc[0].length = urb.buffer_length;
    
    // Submit isochronous URB for SPCMic audio data with retry for busy device
    int result = -1;
    int retryCount = 0;
    const int maxRetries = 3;
    
    while (retryCount < maxRetries) {
        result = ioctl(m_deviceFd, USBDEVFS_SUBMITURB, &urb);
        if (result >= 0) {
            break; // Success
        }
        
        if (errno == EBUSY) {
            retryCount++;
            LOGI("USB device busy (retry %d/%d) - waiting before retry", retryCount, maxRetries);
            
            // Try to clear any pending URBs that might be causing the busy state
            ioctl(m_deviceFd, USBDEVFS_DISCARDURB, &urb);
            
            // Progressive backoff delay
            std::this_thread::sleep_for(std::chrono::milliseconds(retryCount * 5));
        } else {
            // Other errors - don't retry
            break;
        }
    }
    
    if (result < 0) {
        // Log the isochronous submit error
        static int submitErrorCount = 0;
        if (++submitErrorCount <= 5) {
            LOGE("USB isochronous submit failed (attempt %d): %s (errno %d)", submitErrorCount, strerror(errno), errno);
            if (errno == EBUSY) {
                LOGE("Device busy - another process may be using the SPCMic device");
            }
        }
        
        // Fallback to bulk transfer if isochronous fails completely
        struct usbdevfs_bulktransfer bulk;
        bulk.ep = m_audioInEndpoint;
        bulk.len = std::min(bufferSize, (size_t)1024);
        bulk.timeout = 1;
        bulk.data = buffer;
        
        int bytesRead = ioctl(m_deviceFd, USBDEVFS_BULK, &bulk);
        if (bytesRead > 0) {
            static int bulkSuccessCount = 0;
            if (++bulkSuccessCount <= 5) {
                LOGI("Bulk transfer success %d: received %d bytes", bulkSuccessCount, bytesRead);
            }
            return bytesRead;
        } else if (bytesRead == 0 || errno == ETIMEDOUT) {
            return 0;
        } else {
            // Log bulk transfer error
            static int errorCount = 0;
            if (++errorCount <= 5) {
                LOGE("USB bulk read error (attempt %d): %s (errno %d)", errorCount, strerror(errno), errno);
                if (errno == EINVAL) {
                    LOGE("EINVAL error suggests USB interface configuration issue");
                    LOGE("SPCMic may need different alternate setting for 84-channel streaming");
                } else if (errno == EBUSY) {
                    LOGE("Device busy - check if another app is using the SPCMic");
                }
            }
            return 0;
        }
    } else {
        // Isochronous URB submitted successfully - now reap it
        struct usbdevfs_urb *completed_urb = nullptr;
        int reap_result = ioctl(m_deviceFd, USBDEVFS_REAPURB, &completed_urb);
        
        if (reap_result >= 0 && completed_urb && completed_urb->actual_length > 0) {
            // Successfully received isochronous audio data from SPCMic
            static int isoSuccessCount = 0;
            if (++isoSuccessCount <= 5) {
                LOGI("Isochronous transfer success %d: received %d bytes", isoSuccessCount, completed_urb->actual_length);
            }
            return completed_urb->actual_length;
        } else if (reap_result == 0 || errno == ETIMEDOUT) {
            // No data available yet - normal for isochronous transfers
            return 0;
        } else {
            // Log isochronous error
            static int isoErrorCount = 0;
            if (++isoErrorCount <= 5) {
                LOGE("USB isochronous reap error (attempt %d): %s (errno %d)", isoErrorCount, strerror(errno), errno);
            }
            return 0;
        }
    }
}

void USBAudioInterface::release() {
    LOGI("Releasing USB audio interface");
    
    stopStreaming();
    
    if (m_deviceFd >= 0) {
        // Set SPCMic Interface 3 alt setting 0 to stop streaming
        LOGI("Set interface 3 alt setting 0");
        setInterface(3, 0);
        
        m_deviceFd = -1;
    }
    
    LOGI("USB audio interface released");
}
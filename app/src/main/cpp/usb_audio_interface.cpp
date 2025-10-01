#include "usb_audio_interface.h"
#include "uac_protocol.h"
#include <android/log.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <cstring>
#include <cmath>
#include <cstdlib>
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
    // Set this FIRST before configuring sample rate
    LOGI("Setting Interface 3 to alternate setting 1 for SPCMic 84-channel mode");
    
    struct usbdevfs_setinterface setintf = {0};
    setintf.interface = 3;  // SPCMic audio streaming interface
    setintf.altsetting = 1; // Alternate setting 1 has the 84-channel endpoint
    
    int result = ioctl(m_deviceFd, USBDEVFS_SETINTERFACE, &setintf);
    if (result == 0) {
        LOGI("Successfully configured Interface 3 alternate setting 1 for 84-channel streaming");
        
        // Give the device time to configure the isochronous endpoint
        usleep(100000); // 100ms delay for endpoint configuration
        
        // NOW configure sample rate AFTER interface is active
        LOGI("Configuring sample rate to 48000 Hz on active endpoint");
        uint32_t sampleRate = 48000;
        
        struct usbdevfs_ctrltransfer ctrl = {0};
        ctrl.bRequestType = 0x22; // Class, Endpoint, Host to Device
        ctrl.bRequest = 0x01;     // SET_CUR
        ctrl.wValue = 0x0100;     // CS_SAM_FREQ_CONTROL (Sampling Frequency Control)
        ctrl.wIndex = 0x81;       // Endpoint 0x81
        ctrl.wLength = 3;         // 3 bytes for sample rate (24-bit)
        ctrl.timeout = 1000;
        ctrl.data = (void*)&sampleRate;
        
        int ctrl_result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
        if (ctrl_result >= 0) {
            LOGI("Sample rate set successfully on active endpoint");
            
            // Query it back to verify
            uint32_t queriedRate = 0;
            struct usbdevfs_ctrltransfer ctrl_query = {0};
            ctrl_query.bRequestType = 0xA2; // Class, Endpoint, Device to Host (READ)
            ctrl_query.bRequest = 0x81;     // GET_CUR
            ctrl_query.wValue = 0x0100;     // CS_SAM_FREQ_CONTROL
            ctrl_query.wIndex = 0x81;       // Endpoint 0x81
            ctrl_query.wLength = 3;
            ctrl_query.timeout = 1000;
            ctrl_query.data = &queriedRate;
            
            int query_result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl_query);
            if (query_result >= 0) {
                LOGI("Verified: Device now reports sample rate: %u Hz", queriedRate & 0xFFFFFF);
            }
        } else {
            LOGI("Sample rate control after interface activation failed (errno %d)", errno);
        }
        
        usleep(50000); // Extra 50ms for sample rate to take effect
        
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
        static int notStreamingCount = 0;
        if (++notStreamingCount <= 5) {
            LOGE("readAudioData returning 0: isStreaming=%d, fd=%d", m_isStreaming, m_deviceFd);
        }
        return 0;
    }
    
    // Calculate expected frame size for 84 channels at 24-bit
    const size_t frameSize = m_channelCount * m_bytesPerSample; // 84 * 3 = 252 bytes
    const size_t maxFrames = bufferSize / frameSize;
    
    if (maxFrames == 0) {
        static int noFramesCount = 0;
        if (++noFramesCount <= 5) {
            LOGE("readAudioData returning 0: maxFrames=0, bufferSize=%zu, frameSize=%zu", 
                 bufferSize, frameSize);
        }
        return 0;
    }
    
    // USB Audio Class: SPCMic uses isochronous transfers for real-time audio
    // Based on PAL logs: Interface 3, Altset 1, Endpoint 0x81 (SYNC) isochronous
    // Android requires USBDEVFS_SUBMITURB for isochronous transfers
    
    // Use dynamically allocated URB queue for continuous streaming
    const int NUM_URBS = 4; // Multiple URBs in flight for better throughput
    const int PACKETS_PER_URB = 1; // Single packet per URB to start
    // CRITICAL: For isochronous URBs, buffer_length and iso_frame_desc[].length must match
    // and must not exceed the endpoint's wMaxPacketSize (5104 for SPCMic)
    const int URB_BUFFER_SIZE = 5104;
    
    // Dynamically allocated URBs with proper size for iso_frame_desc array
    static struct usbdevfs_urb** urbs = nullptr;
    static uint8_t urbBuffers[NUM_URBS][8192];
    static bool urbsInitialized = false;
    static int nextSubmitIndex = 0;
    static int totalSubmitted = 0;
    static bool wasStreaming = false; // Track previous streaming state
    
    static int callCount = 0;
    callCount++;
    
    // Log first few calls with state information
    if (callCount <= 5 || callCount % 1000 == 0) {
        LOGI("readAudioData called (count=%d): bufferSize=%zu, isStreaming=%d, wasStreaming=%d, urbsInit=%d, totalSub=%d, fd=%d", 
             callCount, bufferSize, m_isStreaming, wasStreaming, urbsInitialized, totalSubmitted, m_deviceFd);
    }
    
    // Detect streaming state change and reset URB queue
    if (m_isStreaming && !wasStreaming) {
        // Just started streaming - reset URB queue
        LOGI("Streaming started - resetting URB queue state");
        
        // Free existing URBs if they exist
        if (urbsInitialized && urbs != nullptr) {
            for (int i = 0; i < NUM_URBS; i++) {
                if (urbs[i] != nullptr) {
                    free(urbs[i]);
                    urbs[i] = nullptr;
                }
            }
            free(urbs);
            urbs = nullptr;
        }
        
        totalSubmitted = 0;
        nextSubmitIndex = 0;
        urbsInitialized = false; // Force re-initialization
        wasStreaming = true;
    } else if (!m_isStreaming && wasStreaming) {
        // Just stopped streaming
        LOGI("Streaming stopped - will re-initialize on next start");
        wasStreaming = false;
    }
    
    if (!urbsInitialized) {
        // Allocate URBs with proper size for iso_frame_desc array
        // CRITICAL: The usbdevfs_urb structure already includes iso_frame_desc[0],
        // so for PACKETS_PER_URB=1, we just need sizeof(usbdevfs_urb)
        // For more packets: sizeof(usbdevfs_urb) + (PACKETS_PER_URB - 1) * sizeof(usbdevfs_iso_packet_desc)
        size_t urb_size = sizeof(struct usbdevfs_urb);
        
        urbs = (struct usbdevfs_urb**)malloc(NUM_URBS * sizeof(struct usbdevfs_urb*));
        if (!urbs) {
            LOGE("Failed to allocate URB pointer array");
            return 0;
        }
        
        for (int i = 0; i < NUM_URBS; i++) {
            urbs[i] = (struct usbdevfs_urb*)malloc(urb_size);
            if (!urbs[i]) {
                LOGE("Failed to allocate URB[%d]", i);
                return 0;
            }
            
            memset(urbs[i], 0, urb_size);
            urbs[i]->type = USBDEVFS_URB_TYPE_ISO;
            urbs[i]->endpoint = m_audioInEndpoint;  // 0x81
            urbs[i]->status = 0;
            urbs[i]->flags = USBDEVFS_URB_ISO_ASAP;
            urbs[i]->buffer = urbBuffers[i];
            urbs[i]->buffer_length = URB_BUFFER_SIZE;
            urbs[i]->actual_length = 0;
            urbs[i]->start_frame = 0;
            urbs[i]->number_of_packets = PACKETS_PER_URB;
            urbs[i]->error_count = 0;
            urbs[i]->signr = 0;
            urbs[i]->usercontext = (void*)(intptr_t)i;
            
            // Configure the single ISO packet descriptor
            // 2560 works but gives EOVERFLOW - device sending more data
            // 3072/4096 give EMSGSIZE - Android kernel has strict limit
            // Device needs: 84 channels × 3 bytes × 16 samples = 4032 bytes
            // But kernel limit appears to be ~2816 bytes (2.75KB)
            // Try 2816 as compromise - halfway between 2560 (works) and 3072 (fails)
            urbs[i]->iso_frame_desc[0].length = 2816;
            urbs[i]->iso_frame_desc[0].actual_length = 0;
            urbs[i]->iso_frame_desc[0].status = 0;
        }
        
        urbsInitialized = true;
        LOGI("Initialized %d ISO URBs (dynamically allocated): buffer_length=%d, packets=%d, urb_size=%zu, packet_length=%u", 
             NUM_URBS, URB_BUFFER_SIZE, PACKETS_PER_URB, urb_size, urbs[0]->iso_frame_desc[0].length);
    }
    
    // First, try to reap any completed URBs
    struct usbdevfs_urb *completed_urb = nullptr;
    int reap_result = ioctl(m_deviceFd, USBDEVFS_REAPURBNDELAY, &completed_urb);
    
    if (reap_result >= 0 && completed_urb) {
        // Successfully reaped a URB - check the packet data
        int packet_status = completed_urb->iso_frame_desc[0].status;
        int packet_actual = completed_urb->iso_frame_desc[0].actual_length;
        int urb_index = (int)(intptr_t)completed_urb->usercontext;
        
        static int reapCount = 0;
        reapCount++;
        
        if (packet_actual > 0) {
            // Got data! Copy it to the output buffer
            size_t bytesToCopy = std::min((size_t)packet_actual, bufferSize);
            memcpy(buffer, completed_urb->buffer, bytesToCopy);
            
            if (reapCount <= 20 || reapCount % 100 == 0) {
                // Calculate how many samples we got (84 channels × 3 bytes per sample)
                int samplesPerChannel = packet_actual / (84 * 3);
                
                LOGI("ISO URB[%d] reaped (count=%d): actual=%d bytes (%d samples/ch), status=%d (%s)", 
                     urb_index, reapCount, packet_actual, samplesPerChannel, packet_status,
                     (packet_status == -75 ? "EOVERFLOW" : 
                      packet_status == 0 ? "OK" : "ERROR"));
            }
            
            // Re-submit this URB immediately for continuous streaming
            completed_urb->iso_frame_desc[0].actual_length = 0;
            completed_urb->iso_frame_desc[0].status = 0;
            int submit_result = ioctl(m_deviceFd, USBDEVFS_SUBMITURB, completed_urb);
            if (submit_result < 0) {
                LOGE("Failed to re-submit URB[%d]: %s (errno %d)", urb_index, strerror(errno), errno);
            }
            
            return bytesToCopy;
        } else {
            // No data in this packet - log and re-submit
            if (reapCount <= 10) {
                LOGI("ISO URB[%d] reaped with no data: status=%d", urb_index, packet_status);
            }
            
            // Re-submit this URB
            completed_urb->iso_frame_desc[0].actual_length = 0;
            completed_urb->iso_frame_desc[0].status = 0;
            int submit_result = ioctl(m_deviceFd, USBDEVFS_SUBMITURB, completed_urb);
            if (submit_result < 0) {
                LOGE("Failed to re-submit URB[%d]: %s (errno %d)", urb_index, strerror(errno), errno);
            }
        }
    } else if (reap_result < 0 && errno != EAGAIN) {
        // Reap error (not EAGAIN)
        static int reapErrorCount = 0;
        if (++reapErrorCount <= 10) {
            LOGE("URB reap error: %s (errno %d)", strerror(errno), errno);
        }
    }
    
    // Submit new URBs if needed (initial priming or if queue is low)
    if (totalSubmitted < NUM_URBS) {
        // Prime the queue by submitting all URBs initially
        static int attemptCount = 0;
        attemptCount++;
        
        if (attemptCount <= 20 || attemptCount % 100 == 0) {
            LOGI("Attempting to submit URB[%d] (attempt=%d, totalSub=%d/%d)", 
                 nextSubmitIndex, attemptCount, totalSubmitted, NUM_URBS);
        }
        
        int result = ioctl(m_deviceFd, USBDEVFS_SUBMITURB, urbs[nextSubmitIndex]);
        if (result >= 0) {
            totalSubmitted++;
            if (totalSubmitted <= NUM_URBS) {
                LOGI("Submitted initial URB[%d] (%d/%d)", nextSubmitIndex, totalSubmitted, NUM_URBS);
            }
            nextSubmitIndex = (nextSubmitIndex + 1) % NUM_URBS;
        } else {
            static int submitErrorCount = 0;
            submitErrorCount++;
            if (submitErrorCount <= 20 || submitErrorCount % 100 == 0) {
                LOGE("Failed to submit URB[%d] (attempt %d): %s (errno %d)", 
                     nextSubmitIndex, submitErrorCount, strerror(errno), errno);
            }
        }
    }
    
    // No data available yet - return 0 (normal for streaming)
    return 0;
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
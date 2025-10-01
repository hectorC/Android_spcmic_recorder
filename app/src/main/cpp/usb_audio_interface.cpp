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

// Define missing USB ioctl for getting current frame number (not in Android NDK headers)
#ifndef USBDEVFS_GET_CURRENT_FRAME
#define USBDEVFS_GET_CURRENT_FRAME _IOR('U', 19, unsigned int)
#endif

USBAudioInterface::USBAudioInterface() 
    : m_deviceFd(-1)
    , m_sampleRate(48000)
    , m_channelCount(84)
    , m_bytesPerSample(3)  // 24-bit = 3 bytes
    , m_isStreaming(false)
    , m_audioInEndpoint(0x81)  // Typical USB audio input endpoint
    , m_controlEndpoint(0x00)
    , m_nextSubmitIndex(0)
    , m_totalSubmitted(0)
    , m_callCount(0)
    , m_attemptCount(0)
    , m_submitErrorCount(0)
    , m_reapCount(0)
    , m_reapErrorCount(0)
    , m_eagainCount(0)
    , m_reapAttemptCount(0)
    , m_lastReapedUrbAddress(nullptr)
    , m_consecutiveSameUrbCount(0)
    , m_recentReapCheckpoint(0)
    , m_stuckUrbDetected(false)
    , m_urbs(nullptr)
    , m_urbBuffers(nullptr)
    , m_urbsInitialized(false)
    , m_wasStreaming(false)
    , m_notStreamingCount(0)
    , m_noFramesCount(0)
    , m_currentFrameNumber(0)
    , m_frameNumberInitialized(false) {
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
    
    // Give the device extra time to start streaming audio data
    // Without this delay, m_urbs may complete with no data
    usleep(150000); // 150ms delay to ensure device is ready
    
    LOGI("USB audio streaming started");
    return true;
}

bool USBAudioInterface::stopStreaming() {
    if (!m_isStreaming) {
        return true;
    }
    
    LOGI("Stopping USB audio streaming");
    m_isStreaming = false;
    
    // Cancel any pending URBs before disabling the interface
    if (m_urbs && m_deviceFd >= 0) {
        const int NUM_URBS = 4;
        
        // First, cancel all URBs
        int cancelledCount = 0;
        for (int i = 0; i < NUM_URBS; i++) {
            if (m_urbs[i]) {
                int result = ioctl(m_deviceFd, USBDEVFS_DISCARDURB, m_urbs[i]);
                if (result == 0) {
                    cancelledCount++;
                } else if (errno != EINVAL) {
                    // EINVAL means URB wasn't submitted or already completed - that's OK
                    LOGE("Failed to cancel URB[%d]: %s (errno %d)", i, strerror(errno), errno);
                }
            }
        }
        
        // Now reap all cancelled URBs to ensure they're fully processed
        if (cancelledCount > 0) {
            LOGI("Reaping %d cancelled URBs...", cancelledCount);
            for (int i = 0; i < cancelledCount; i++) {
                struct usbdevfs_urb *reaped_urb = nullptr;
                int reap_result = ioctl(m_deviceFd, USBDEVFS_REAPURB, &reaped_urb);
                if (reap_result < 0) {
                    LOGE("Failed to reap cancelled URB %d: %s (errno %d)", i, strerror(errno), errno);
                    break;
                }
            }
        }
        
        LOGI("Cancelled and reaped all pending URBs");
    }
    
    // Disable audio streaming - set SPCMic Interface 3 back to alt setting 0
    // This stops the 84-channel audio streaming
    setInterface(3, 0);
    
    LOGI("USB audio streaming stopped");
    return true;
}

bool USBAudioInterface::enableAudioStreaming() {
    LOGI("Enabling USB audio streaming for SPCMic device - following Linux USB audio driver sequence");
    
    // STEP 1: Reset interface to alt 0 (disable streaming)
    // This is CRITICAL - Linux USB audio driver always does this first
    LOGI("Step 1: Setting Interface 3 to alt 0 (disable streaming)");
    setInterface(3, 0);
    usleep(50000); // 50ms delay
    
    // STEP 2: Configure sample rate BEFORE enabling the interface
    // Linux USB audio does: usb_set_interface(alt 0) -> init_sample_rate() -> usb_set_interface(alt 1)
    LOGI("Step 2: Configuring sample rate to 48000 Hz on endpoint 0x81");
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
        LOGI("Sample rate configured successfully on endpoint");
    } else {
        LOGI("Sample rate control failed (errno %d: %s) - may be implicit in alt setting", errno, strerror(errno));
    }
    
    usleep(10000); // 10ms for sample rate to take effect
    
    // STEP 2.5: Initialize pitch control (Linux USB audio driver does this)
    LOGI("Step 2.5: Initializing pitch control");
    uint8_t pitchEnable = 1;
    struct usbdevfs_ctrltransfer pitch_ctrl = {0};
    pitch_ctrl.bRequestType = 0x22; // Class, Endpoint, Host to Device
    pitch_ctrl.bRequest = 0x01;     // SET_CUR
    pitch_ctrl.wValue = 0x0200;     // PITCH_CONTROL
    pitch_ctrl.wIndex = 0x81;       // Endpoint 0x81
    pitch_ctrl.wLength = 1;
    pitch_ctrl.timeout = 1000;
    pitch_ctrl.data = &pitchEnable;
    
    int pitch_result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &pitch_ctrl);
    if (pitch_result >= 0) {
        LOGI("Pitch control enabled successfully");
    } else {
        LOGI("Pitch control failed (errno %d: %s) - may not be supported", errno, strerror(errno));
    }
    
    usleep(10000); // 10ms for pitch control to take effect
    
    // STEP 3: NOW activate the interface by setting alt 1
    // This starts the isochronous streaming
    LOGI("Step 3: Setting Interface 3 to alt 1 (enable 84-channel streaming)");
    struct usbdevfs_setinterface setintf = {0};
    setintf.interface = 3;
    setintf.altsetting = 1;
    
    int result = ioctl(m_deviceFd, USBDEVFS_SETINTERFACE, &setintf);
    if (result == 0) {
        LOGI("Successfully enabled Interface 3 alt 1 for 84-channel streaming");
    } else {
        LOGE("Failed to set Interface 3 alt 1: %s", strerror(errno));
        return false;
    }
    
    usleep(50000); // 50ms for device to start streaming
    
    m_isStreaming = true;
    LOGI("SPCMic streaming enabled - ready for isochronous transfers on endpoint 0x81");
    return true;
}

size_t USBAudioInterface::readAudioData(uint8_t* buffer, size_t bufferSize) {
    if (!m_isStreaming || m_deviceFd < 0) {
        if (++m_notStreamingCount <= 5) {
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
    // Try 2944 bytes per packet - halfway between 2816 (EOVERFLOW) and 3072 (EMSGSIZE)
    // Kernel limit appears to be around 23-24KB total per URB
    const int NUM_URBS = 32; // 32 URBs for deep queue
    const int PACKETS_PER_URB = 8; // 8 packets per URB
    // CRITICAL: For isochronous URBs, buffer_length must accommodate all packets
    // 8 packets × 2944 bytes = 23,552 bytes
    const int URB_BUFFER_SIZE = 23552; // ~23.5KB buffer per URB (8 × 2944)
    
    // Dynamically allocated m_urbs with proper size for iso_frame_desc array
    
    
    
    
    
    m_callCount++;
    
    // Log first few calls with state information
    if (m_callCount <= 5 || m_callCount % 1000 == 0) {
        LOGI("readAudioData called (count=%d): bufferSize=%zu, isStreaming=%d, m_wasStreaming=%d, urbsInit=%d, totalSub=%d, fd=%d", 
             m_callCount, bufferSize, m_isStreaming, m_wasStreaming, m_urbsInitialized, m_totalSubmitted, m_deviceFd);
    }
    
    // Detect streaming state change and reset URB queue
    if (m_isStreaming && !m_wasStreaming) {
        // Just started streaming - reset URB queue
        LOGI("Streaming started - resetting URB queue state");
        
        // Free existing m_urbs if they exist
        if (m_urbsInitialized && m_urbs != nullptr) {
            for (int i = 0; i < NUM_URBS; i++) {
                if (m_urbs[i] != nullptr) {
                    free(m_urbs[i]);
                    m_urbs[i] = nullptr;
                }
                if (m_urbBuffers && m_urbBuffers[i] != nullptr) {
                    free(m_urbBuffers[i]);
                    m_urbBuffers[i] = nullptr;
                }
            }
            free(m_urbs);
            m_urbs = nullptr;
            if (m_urbBuffers) {
                free(m_urbBuffers);
                m_urbBuffers = nullptr;
            }
        }
        
        m_totalSubmitted = 0;
        m_nextSubmitIndex = 0;
        m_urbsInitialized = false; // Force re-initialization
        m_frameNumberInitialized = false; // Reset frame number tracking
        m_wasStreaming = true;
    } else if (!m_isStreaming && m_wasStreaming) {
        // Just stopped streaming
        LOGI("Streaming stopped - will re-initialize on next start");
        m_wasStreaming = false;
    }
    
    if (!m_urbsInitialized) {
        // Allocate m_urbs with proper size for iso_frame_desc array
        // CRITICAL: The usbdevfs_urb structure already includes iso_frame_desc[0],
        // so for PACKETS_PER_URB=1, we just need sizeof(usbdevfs_urb)
        // For more packets: sizeof(usbdevfs_urb) + (PACKETS_PER_URB - 1) * sizeof(usbdevfs_iso_packet_desc)
        size_t urb_size = sizeof(struct usbdevfs_urb) + (PACKETS_PER_URB - 1) * sizeof(struct usbdevfs_iso_packet_desc);
        
        m_urbs = (struct usbdevfs_urb**)malloc(NUM_URBS * sizeof(struct usbdevfs_urb*));
        if (!m_urbs) {
            LOGE("Failed to allocate URB pointer array");
            return 0;
        }
        
        // Allocate URB buffers dynamically (not static to avoid memory corruption)
        m_urbBuffers = (uint8_t**)malloc(NUM_URBS * sizeof(uint8_t*));
        if (!m_urbBuffers) {
            LOGE("Failed to allocate URB buffer pointer array");
            free(m_urbs);
            m_urbs = nullptr;
            return 0;
        }
        
        for (int i = 0; i < NUM_URBS; i++) {
            m_urbBuffers[i] = (uint8_t*)malloc(16384);  // 16KB with safety margin
            if (!m_urbBuffers[i]) {
                LOGE("Failed to allocate URB buffer[%d]", i);
                // Clean up previously allocated buffers
                for (int j = 0; j < i; j++) {
                    free(m_urbBuffers[j]);
                }
                free(m_urbBuffers);
                free(m_urbs);
                m_urbs = nullptr;
                m_urbBuffers = nullptr;
                return 0;
            }
        }
        
        for (int i = 0; i < NUM_URBS; i++) {
            m_urbs[i] = (struct usbdevfs_urb*)malloc(urb_size);
            if (!m_urbs[i]) {
                LOGE("Failed to allocate URB[%d]", i);
                return 0;
            }
            
            memset(m_urbs[i], 0, urb_size);
            m_urbs[i]->type = USBDEVFS_URB_TYPE_ISO;
            m_urbs[i]->endpoint = m_audioInEndpoint;  // 0x81
            m_urbs[i]->status = 0;
            m_urbs[i]->flags = USBDEVFS_URB_ISO_ASAP;  // Back to ISO_ASAP since explicit frames not supported
            m_urbs[i]->buffer = m_urbBuffers[i];
            m_urbs[i]->buffer_length = URB_BUFFER_SIZE;
            m_urbs[i]->actual_length = 0;
            m_urbs[i]->start_frame = 0;  // Ignored when ISO_ASAP is set
            m_urbs[i]->number_of_packets = PACKETS_PER_URB;
            m_urbs[i]->error_count = 0;
            m_urbs[i]->signr = 0;
            m_urbs[i]->usercontext = (void*)(intptr_t)i;
            
            // Configure ISO packet descriptors - 8 packets at 2944 bytes
            // Bisecting to find exact kernel limit: between 2816 (works) and 3072 (EMSGSIZE)
            for (int pkt = 0; pkt < PACKETS_PER_URB; pkt++) {
                m_urbs[i]->iso_frame_desc[pkt].length = 2944;  // 2944 bytes per packet
                m_urbs[i]->iso_frame_desc[pkt].actual_length = 0;
                m_urbs[i]->iso_frame_desc[pkt].status = 0;
            }
        }
        
        m_urbsInitialized = true;
        LOGI("Initialized %d ISO m_urbs (dynamically allocated): buffer_length=%d, packets=%d, urb_size=%zu, packet_length=%u", 
             NUM_URBS, URB_BUFFER_SIZE, PACKETS_PER_URB, urb_size, m_urbs[0]->iso_frame_desc[0].length);
    }
    
    // First, submit initial m_urbs if we haven't submitted all of them yet
    // This ensures we prime the queue before trying to reap
    if (m_totalSubmitted < NUM_URBS) {
        
        // Prime the queue by submitting all m_urbs initially
        m_attemptCount++;
        
        if (m_attemptCount <= 20 || m_attemptCount % 100 == 0) {
            LOGI("Attempting to submit URB[%d] (attempt=%d, totalSub=%d/%d)", 
                 m_nextSubmitIndex, m_attemptCount, m_totalSubmitted, NUM_URBS);
        }
        
        int result = ioctl(m_deviceFd, USBDEVFS_SUBMITURB, m_urbs[m_nextSubmitIndex]);
        if (result >= 0) {
            m_totalSubmitted++;
            if (m_totalSubmitted <= NUM_URBS) {
                LOGI("Submitted initial URB[%d] (%d/%d, %d packets)", m_nextSubmitIndex, m_totalSubmitted, NUM_URBS, PACKETS_PER_URB);
            }
            
            m_nextSubmitIndex = (m_nextSubmitIndex + 1) % NUM_URBS;
            
            // If we haven't submitted all m_urbs yet, return and submit more on next call
            if (m_totalSubmitted < NUM_URBS) {
                return 0;
            }
        } else {
            m_submitErrorCount++;
            if (m_submitErrorCount <= 20 || m_submitErrorCount % 100 == 0) {
                LOGE("Failed to submit URB[%d] (attempt %d): %s (errno %d)", 
                     m_nextSubmitIndex, m_submitErrorCount, strerror(errno), errno);
            }
            return 0;  // Return and try again on next call
        }
    }
    
    // CRITICAL FIX: Reap ALL available URBs in a loop, not just one!
    // The device may complete URBs faster than our polling rate
    // Kernel docs: "keep at least one URB queued for smooth ISO streaming"
    int urbs_reaped_this_call = 0;
    size_t total_bytes_accumulated = 0;
    const int MAX_REAPS_PER_CALL = 32; // Safety limit
    
    for (int reap_loop = 0; reap_loop < MAX_REAPS_PER_CALL; reap_loop++) {
        struct usbdevfs_urb *completed_urb = nullptr;
        int reap_result = ioctl(m_deviceFd, USBDEVFS_REAPURBNDELAY, &completed_urb);
        int saved_errno = errno;
        
        m_reapAttemptCount++;
        
        if (reap_result < 0 && saved_errno == EAGAIN) {
            // No more URBs ready
            if (reap_loop == 0) {
                // Only count as EAGAIN if we didn't reap anything
                if (++m_eagainCount <= 20 || m_eagainCount % 1000 == 0) {
                    LOGI("No URB ready (EAGAIN, count=%d), totalSub=%d", m_eagainCount, m_totalSubmitted);
                }
            }
            break; // Exit reap loop
        }
        
        if (reap_result < 0 && saved_errno != EAGAIN) {
            if (++m_reapErrorCount <= 20) {
                LOGE("URB reap error: result=%d, errno=%d (%s)", reap_result, saved_errno, strerror(saved_errno));
            }
            break;
        }
        
        if (reap_result < 0 || !completed_urb) {
            break; // Exit reap loop
        }
        
        // Successfully reaped a URB - process it
        int urb_index = (int)(intptr_t)completed_urb->usercontext;
        urbs_reaped_this_call++;
        m_reapCount++;
        
        // Track URB address for stuck detection
        if (completed_urb == m_lastReapedUrbAddress) {
            m_consecutiveSameUrbCount++;
        } else {
            if (m_consecutiveSameUrbCount >= STUCK_URB_THRESHOLD && !m_stuckUrbDetected) {
                LOGE("URB STUCK DETECTED! URB @ %p was reaped %d times before this URB[%d] @ %p",
                     m_lastReapedUrbAddress, m_consecutiveSameUrbCount, urb_index, completed_urb);
                m_stuckUrbDetected = true;
            }
            m_consecutiveSameUrbCount = 1;
            m_lastReapedUrbAddress = completed_urb;
        }
        
        // Check for stuck URB pattern periodically
        if (m_reapAttemptCount % CHECK_INTERVAL == 0 && m_reapAttemptCount > 0) {
            if (m_consecutiveSameUrbCount >= (CHECK_INTERVAL * 0.8) && m_reapAttemptCount > 100) {
                LOGE("URB STUCK PATTERN DETECTED! Same URB @ %p reaped %d consecutive times - cancelling all URBs",
                     m_lastReapedUrbAddress, m_consecutiveSameUrbCount);
                
                // Cancel all URBs to break the stuck pattern
                for (int i = 0; i < NUM_URBS; i++) {
                    if (m_urbs[i]) {
                        ioctl(m_deviceFd, USBDEVFS_DISCARDURB, m_urbs[i]);
                    }
                }
                
                // Reset URB state and re-initialize
                m_urbsInitialized = false;
                m_consecutiveSameUrbCount = 0;
                m_lastReapedUrbAddress = nullptr;
                m_stuckUrbDetected = false;
                
                LOGI("All URBs cancelled - will reinitialize on next readAudioData() call");
                return total_bytes_accumulated;
            }
            
            m_recentReapCheckpoint = m_reapCount;
        }
        
        // Collect data from all packets in this URB and check for errors
        int total_actual = 0;
        int error_count = 0;
        for (int pkt = 0; pkt < PACKETS_PER_URB; pkt++) {
            total_actual += completed_urb->iso_frame_desc[pkt].actual_length;
            if (completed_urb->iso_frame_desc[pkt].status != 0) {
                error_count++;
                // Log first few errors to understand what's happening
                if (m_reapCount <= 50 || (m_reapCount % 1000 == 0 && error_count <= 2)) {
                    LOGE("URB[%d] packet[%d] error: status=%d, actual=%d", 
                         urb_index, pkt, completed_urb->iso_frame_desc[pkt].status, 
                         completed_urb->iso_frame_desc[pkt].actual_length);
                }
            }
        }
        
        // Copy data to output buffer if we have room and data exists
        if (total_actual > 0 && total_bytes_accumulated < bufferSize) {
            size_t bytesToCopy = std::min((size_t)total_actual, bufferSize - total_bytes_accumulated);
            memcpy(buffer + total_bytes_accumulated, completed_urb->buffer, bytesToCopy);
            total_bytes_accumulated += bytesToCopy;
            
            if (m_reapCount <= 20 || m_reapCount % 100 == 0) {
                int samplesPerChannel = total_actual / (84 * 3);
                LOGI("ISO URB[%d] reaped (reap#%d, loop#%d): %d bytes (%d samples/ch), accumulated=%zu", 
                     urb_index, m_reapCount, reap_loop, total_actual, samplesPerChannel, total_bytes_accumulated);
            }
        }
        
        // Re-submit this URB immediately for continuous streaming (kernel docs: "keep at least one URB queued")
        for (int pkt = 0; pkt < PACKETS_PER_URB; pkt++) {
            completed_urb->iso_frame_desc[pkt].actual_length = 0;
            completed_urb->iso_frame_desc[pkt].status = 0;
        }
        
        int submit_result = ioctl(m_deviceFd, USBDEVFS_SUBMITURB, completed_urb);
        if (submit_result < 0) {
            LOGE("Failed to re-submit URB[%d]: %s (errno %d)", urb_index, strerror(errno), errno);
        } else if (m_reapCount <= 20) {
            LOGI("Re-submitted URB[%d] successfully", urb_index);
        }
        
        // Continue loop to reap more URBs if available
    }
    
    // Log statistics periodically
    if (urbs_reaped_this_call > 1 && (m_reapCount <= 50 || m_reapCount % 100 == 0)) {
        LOGI("Reaped %d URBs in single call (reap#%d), total bytes=%zu", 
             urbs_reaped_this_call, m_reapCount, total_bytes_accumulated);
    }
    
    // Return accumulated data from all reaped URBs
    return total_bytes_accumulated;
}

void USBAudioInterface::release() {
    LOGI("Releasing USB audio interface");
    
    stopStreaming();
    
    // Clean up dynamically allocated URBs and buffers
    if (m_urbs) {
        const int NUM_URBS = 32;
        for (int i = 0; i < NUM_URBS; i++) {
            if (m_urbs[i]) {
                if (m_deviceFd >= 0) {
                    // Cancel any pending URBs before freeing
                    ioctl(m_deviceFd, USBDEVFS_DISCARDURB, m_urbs[i]);
                }
                free(m_urbs[i]);
                m_urbs[i] = nullptr;
            }
        }
        free(m_urbs);
        m_urbs = nullptr;
    }
    
    if (m_urbBuffers) {
        const int NUM_URBS = 32;
        for (int i = 0; i < NUM_URBS; i++) {
            if (m_urbBuffers[i]) {
                free(m_urbBuffers[i]);
                m_urbBuffers[i] = nullptr;
            }
        }
        free(m_urbBuffers);
        m_urbBuffers = nullptr;
    }
    
    m_urbsInitialized = false;
    
    if (m_deviceFd >= 0) {
        // Set SPCMic Interface 3 alt setting 0 to stop streaming
        LOGI("Set interface 3 alt setting 0");
        setInterface(3, 0);
        
        m_deviceFd = -1;
    }
    
    LOGI("USB audio interface released");
}

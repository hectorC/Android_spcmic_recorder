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
#include <vector>

#define LOG_TAG "USBAudioInterface"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Define missing USB ioctl for getting current frame number (not in Android NDK headers)
#ifndef USBDEVFS_GET_CURRENT_FRAME
#define USBDEVFS_GET_CURRENT_FRAME _IOR('U', 19, unsigned int)
#endif

#ifndef USB_DT_CONFIG
#define USB_DT_CONFIG 0x02
#endif
#ifndef USB_DT_INTERFACE
#define USB_DT_INTERFACE 0x04
#endif
#ifndef USB_DT_ENDPOINT
#define USB_DT_ENDPOINT 0x05
#endif
#ifndef USB_DT_SS_ENDPOINT_COMP
#define USB_DT_SS_ENDPOINT_COMP 0x30
#endif
#ifndef USB_REQ_GET_DESCRIPTOR
#define USB_REQ_GET_DESCRIPTOR 0x06
#endif
#ifndef USB_DIR_IN
#define USB_DIR_IN 0x80
#endif
#ifndef USB_TYPE_STANDARD
#define USB_TYPE_STANDARD 0x00
#endif
#ifndef USB_RECIP_DEVICE
#define USB_RECIP_DEVICE 0x00
#endif
#ifndef USB_ENDPOINT_DIR_MASK
#define USB_ENDPOINT_DIR_MASK 0x80
#endif
#ifndef USB_ENDPOINT_XFERTYPE_MASK
#define USB_ENDPOINT_XFERTYPE_MASK 0x03
#endif
#ifndef USB_ENDPOINT_XFER_ISOC
#define USB_ENDPOINT_XFER_ISOC 0x01
#endif

namespace {

#pragma pack(push, 1)
struct USBDescriptorHeader {
    uint8_t bLength;
    uint8_t bDescriptorType;
};

struct USBConfigDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
};

struct USBInterfaceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
};

struct USBEndpointDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
};

struct USBSSEndpointCompanionDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bMaxBurst;
    uint8_t bmAttributes;
    uint16_t wBytesPerInterval;
};
#pragma pack(pop)

inline uint16_t read_le16(const void* ptr) {
    const uint8_t* bytes = static_cast<const uint8_t*>(ptr);
    return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1] << 8);
}

} // namespace

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
    , m_frameNumberInitialized(false)
    , m_streamInterfaceNumber(-1)
    , m_streamAltSetting(-1)
    , m_isoPacketSize(0)
    , m_packetsPerUrb(0)
    , m_urbBufferSize(0)
    , m_bytesPerInterval(0)
    , m_packetsPerServiceInterval(0)
    , m_endpointInfoReady(false)
    , m_isHighSpeed(false)
    , m_isSuperSpeed(false) {
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
    LOGI("Parsing configuration descriptor to locate audio streaming endpoint");

    std::vector<uint8_t> configDescriptor;
    if (!fetchConfigurationDescriptor(configDescriptor)) {
        LOGE("Failed to fetch configuration descriptor");
        return false;
    }

    if (!parseStreamingEndpoint(configDescriptor)) {
        LOGE("Failed to parse audio streaming endpoint from descriptor");
        return false;
    }

    LOGI("Selected audio streaming interface %d alt %d, endpoint 0x%02x", 
         m_streamInterfaceNumber, m_streamAltSetting, m_audioInEndpoint);
    LOGI("Endpoint characteristics: isoPacketSize=%zu bytes, servicePackets=%zu, bytesPerInterval=%zu", 
         m_isoPacketSize, m_packetsPerServiceInterval, m_bytesPerInterval);

    return true;
}

bool USBAudioInterface::configureUACDevice() {
    LOGI("Configuring USB Audio Class device");
    
    if (!m_endpointInfoReady) {
        LOGE("Cannot configure UAC device before endpoint discovery");
        return false;
    }

    LOGI("Using Android-provided device file descriptor; streaming endpoint 0x%02x on interface %d alt %d", 
         m_audioInEndpoint, m_streamInterfaceNumber, m_streamAltSetting);
    LOGI("Isochronous geometry: isoPacketSize=%zu bytes, servicePackets=%zu, bytesPerInterval=%zu", 
         m_isoPacketSize, m_packetsPerServiceInterval, m_bytesPerInterval);
    
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
    int streamingInterface = (m_streamInterfaceNumber >= 0) ? m_streamInterfaceNumber : 3;
    ctrl.wIndex = static_cast<uint16_t>((streamingInterface << 8) | 0x00); // Clock Source ID assumed 0
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

bool USBAudioInterface::fetchConfigurationDescriptor(std::vector<uint8_t>& descriptor) {
    descriptor.clear();

    if (m_deviceFd < 0) {
        LOGE("Invalid device handle when fetching configuration descriptor");
        return false;
    }

    constexpr size_t MAX_CONFIG_DESCRIPTOR_SIZE = 4096;

    USBConfigDescriptor header = {};
    struct usbdevfs_ctrltransfer ctrl = {};
    ctrl.bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    ctrl.bRequest = USB_REQ_GET_DESCRIPTOR;
    ctrl.wValue = static_cast<uint16_t>((USB_DT_CONFIG << 8) | 0);
    ctrl.wIndex = 0;
    ctrl.wLength = sizeof(header);
    ctrl.timeout = 1000;
    ctrl.data = &header;

    int result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
    if (result < 0) {
        LOGE("Failed to fetch configuration descriptor header: %s", strerror(errno));
        return false;
    }

    uint16_t totalLength = read_le16(&header.wTotalLength);
    if (totalLength < sizeof(USBConfigDescriptor)) {
        LOGE("Configuration descriptor total length too small: %u", totalLength);
        return false;
    }

    size_t fetchLength = std::min(static_cast<size_t>(totalLength), MAX_CONFIG_DESCRIPTOR_SIZE);
    descriptor.resize(fetchLength);

    ctrl.wLength = static_cast<uint16_t>(fetchLength);
    ctrl.data = descriptor.data();

    result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
    if (result < 0) {
        LOGE("Failed to fetch full configuration descriptor: %s", strerror(errno));
        descriptor.clear();
        return false;
    }

    if (fetchLength < totalLength) {
        LOGI("Configuration descriptor truncated from %u to %zu bytes", totalLength, fetchLength);
    }

    return true;
}

bool USBAudioInterface::parseStreamingEndpoint(const std::vector<uint8_t>& descriptor) {
    m_streamInterfaceNumber = -1;
    m_streamAltSetting = -1;
    m_audioInEndpoint = -1;
    m_isoPacketSize = 0;
    m_packetsPerServiceInterval = 0;
    m_bytesPerInterval = 0;
    m_endpointInfoReady = false;
    m_isHighSpeed = false;
    m_isSuperSpeed = false;

    struct EndpointSelection {
        bool valid = false;
        int interfaceNumber = -1;
        int altSetting = -1;
        uint8_t endpointAddress = 0;
        size_t isoPacketSize = 0;
        size_t bytesPerInterval = 0;
        size_t packetsPerServiceInterval = 1;
        bool isSuperSpeed = false;
    };

    EndpointSelection best;
    EndpointSelection current;
    bool inCandidateInterface = false;

    auto evaluateCurrent = [&](const EndpointSelection& candidate) {
        if (!candidate.valid) {
            return;
        }
        if (!best.valid || candidate.bytesPerInterval > best.bytesPerInterval) {
            best = candidate;
        }
    };

    size_t offset = 0;
    while (offset + sizeof(USBDescriptorHeader) <= descriptor.size()) {
        const auto* header = reinterpret_cast<const USBDescriptorHeader*>(&descriptor[offset]);
        if (header->bLength == 0) {
            LOGE("Encountered zero-length USB descriptor at offset %zu", offset);
            break;
        }

        if (offset + header->bLength > descriptor.size()) {
            LOGE("Descriptor overruns buffer at offset %zu (length=%u, total=%zu)", 
                 offset, header->bLength, descriptor.size());
            break;
        }

        switch (header->bDescriptorType) {
            case USB_DT_INTERFACE: {
                const auto* intf = reinterpret_cast<const USBInterfaceDescriptor*>(&descriptor[offset]);
                bool isAudioStreaming = (intf->bInterfaceClass == USB_CLASS_AUDIO &&
                                         intf->bInterfaceSubClass == USB_SUBCLASS_AUDIOSTREAMING);
                inCandidateInterface = isAudioStreaming && (intf->bAlternateSetting > 0);
                current = EndpointSelection{};
                if (inCandidateInterface) {
                    current.interfaceNumber = intf->bInterfaceNumber;
                    current.altSetting = intf->bAlternateSetting;
                    current.packetsPerServiceInterval = 1;
                    LOGI("Inspecting audio streaming interface %d alt %d", 
                         current.interfaceNumber, current.altSetting);
                }
                break;
            }

            case USB_DT_ENDPOINT: {
                if (!inCandidateInterface) {
                    break;
                }

                const auto* ep = reinterpret_cast<const USBEndpointDescriptor*>(&descriptor[offset]);
                uint8_t direction = ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK;
                uint8_t transferType = ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;

                if (direction != USB_ENDPOINT_DIR_MASK || transferType != USB_ENDPOINT_XFER_ISOC) {
                    break; // Not an isochronous IN endpoint
                }

                EndpointSelection candidate = current;
                candidate.endpointAddress = ep->bEndpointAddress;

                uint16_t rawMaxPacket = read_le16(&ep->wMaxPacketSize);
                int basePacketSize = rawMaxPacket & 0x7FF;
                int additionalTransactions = (rawMaxPacket >> 11) & 0x03;
                int transactionsPerService = additionalTransactions + 1;
                size_t payloadPerInterval = static_cast<size_t>(basePacketSize) * transactionsPerService;
                candidate.bytesPerInterval = payloadPerInterval;
                candidate.isoPacketSize = payloadPerInterval;
                candidate.packetsPerServiceInterval = std::max<size_t>(1, static_cast<size_t>(1) << std::min<int>(ep->bInterval ? (ep->bInterval - 1) : 0, 10));
                candidate.valid = (payloadPerInterval > 0);
                candidate.isSuperSpeed = false;

                // Check for SuperSpeed companion descriptor
                size_t nextOffset = offset + header->bLength;
                if (nextOffset + sizeof(USBDescriptorHeader) <= descriptor.size()) {
                    const auto* nextHeader = reinterpret_cast<const USBDescriptorHeader*>(&descriptor[nextOffset]);
                    if (nextHeader->bDescriptorType == USB_DT_SS_ENDPOINT_COMP) {
                        const auto* ss = reinterpret_cast<const USBSSEndpointCompanionDescriptor*>(&descriptor[nextOffset]);
                        size_t burst = static_cast<size_t>(ss->bMaxBurst) + 1;
                        size_t mult = static_cast<size_t>(ss->bmAttributes & 0x07) + 1;
                        size_t bytesPerInterval = read_le16(&ss->wBytesPerInterval);
                        if (bytesPerInterval == 0) {
                            bytesPerInterval = static_cast<size_t>(basePacketSize) * burst * mult;
                        }
                        candidate.bytesPerInterval = bytesPerInterval;
                        candidate.isoPacketSize = bytesPerInterval;
                        candidate.packetsPerServiceInterval = std::max<size_t>(1, static_cast<size_t>(1) << std::min<int>(ep->bInterval ? (ep->bInterval - 1) : 0, 10));
                        candidate.isSuperSpeed = true;
                    }
                }

                if (candidate.valid) {
                    LOGI("Found candidate endpoint 0x%02x (interface %d alt %d): basePacket=%d, transactions=%d, bytesPerInterval=%zu", 
                         candidate.endpointAddress, candidate.interfaceNumber, candidate.altSetting,
                         basePacketSize, transactionsPerService, candidate.bytesPerInterval);
                    evaluateCurrent(candidate);
                }

                break;
            }

            default:
                break;
        }

        offset += header->bLength;
    }

    if (!best.valid) {
        LOGE("No audio streaming endpoint candidates discovered");
        return false;
    }

    m_streamInterfaceNumber = best.interfaceNumber;
    m_streamAltSetting = best.altSetting;
    m_audioInEndpoint = best.endpointAddress;
    m_isoPacketSize = best.isoPacketSize;
    m_bytesPerInterval = best.bytesPerInterval;
    m_packetsPerServiceInterval = std::max<size_t>(1, best.packetsPerServiceInterval);
    m_isSuperSpeed = best.isSuperSpeed;
    m_isHighSpeed = !best.isSuperSpeed;
    m_endpointInfoReady = true;

    LOGI("Selected endpoint 0x%02x: isoPacketSize=%zu, bytesPerInterval=%zu, packetsPerServiceInterval=%zu, superSpeed=%d", 
         m_audioInEndpoint, m_isoPacketSize, m_bytesPerInterval, m_packetsPerServiceInterval, m_isSuperSpeed ? 1 : 0);

    return true;
}

void USBAudioInterface::releaseUrbResources() {
    if (m_urbs) {
        for (int i = 0; i < NUM_URBS; ++i) {
            if (m_urbs[i]) {
                if (m_deviceFd >= 0) {
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
        for (int i = 0; i < NUM_URBS; ++i) {
            if (m_urbBuffers[i]) {
                free(m_urbBuffers[i]);
                m_urbBuffers[i] = nullptr;
            }
        }
        free(m_urbBuffers);
        m_urbBuffers = nullptr;
    }

    m_urbsInitialized = false;
    m_packetsPerUrb = 0;
    m_urbBufferSize = 0;
    m_totalSubmitted = 0;
    m_nextSubmitIndex = 0;
}

bool USBAudioInterface::ensureUrbResources() {
    if (m_urbsInitialized) {
        return true;
    }

    if (!m_endpointInfoReady) {
        LOGE("Cannot allocate URBs before endpoint info is ready");
        return false;
    }

    if (m_isoPacketSize == 0) {
        LOGE("Isochronous packet size not initialized");
        return false;
    }

    size_t packetsPerService = std::max<size_t>(1, m_packetsPerServiceInterval);
    size_t targetPackets = packetsPerService * 8; // Aim for ~8 service intervals per URB
    size_t maxPackets = std::max<size_t>(1, MAX_URB_BUFFER_BYTES / m_isoPacketSize);
    m_packetsPerUrb = std::max<size_t>(1, std::min(targetPackets, maxPackets));
    m_urbBufferSize = m_isoPacketSize * m_packetsPerUrb;

    size_t urbStructSize = sizeof(struct usbdevfs_urb);
    if (m_packetsPerUrb > 1) {
        urbStructSize += (m_packetsPerUrb - 1) * sizeof(struct usbdevfs_iso_packet_desc);
    }

    m_urbs = static_cast<struct usbdevfs_urb**>(calloc(NUM_URBS, sizeof(struct usbdevfs_urb*)));
    if (!m_urbs) {
        LOGE("Failed to allocate URB pointer array");
        return false;
    }

    m_urbBuffers = static_cast<uint8_t**>(calloc(NUM_URBS, sizeof(uint8_t*)));
    if (!m_urbBuffers) {
        LOGE("Failed to allocate URB buffer pointer array");
        free(m_urbs);
        m_urbs = nullptr;
        return false;
    }

    for (int i = 0; i < NUM_URBS; ++i) {
        void* bufferPtr = nullptr;
        if (posix_memalign(&bufferPtr, 64, m_urbBufferSize) != 0) {
            bufferPtr = malloc(m_urbBufferSize);
        }
        if (!bufferPtr) {
            LOGE("Failed to allocate URB buffer %d (%zu bytes)", i, m_urbBufferSize);
            releaseUrbResources();
            return false;
        }
        memset(bufferPtr, 0, m_urbBufferSize);
        m_urbBuffers[i] = static_cast<uint8_t*>(bufferPtr);

        m_urbs[i] = static_cast<struct usbdevfs_urb*>(calloc(1, urbStructSize));
        if (!m_urbs[i]) {
            LOGE("Failed to allocate URB structure %d", i);
            releaseUrbResources();
            return false;
        }

        m_urbs[i]->type = USBDEVFS_URB_TYPE_ISO;
        m_urbs[i]->endpoint = m_audioInEndpoint;
        m_urbs[i]->status = 0;
        m_urbs[i]->flags = USBDEVFS_URB_ISO_ASAP;
        m_urbs[i]->buffer = m_urbBuffers[i];
        m_urbs[i]->buffer_length = m_urbBufferSize;
        m_urbs[i]->actual_length = 0;
        m_urbs[i]->start_frame = 0;
        m_urbs[i]->number_of_packets = static_cast<unsigned>(m_packetsPerUrb);
        m_urbs[i]->error_count = 0;
        m_urbs[i]->signr = 0;
        m_urbs[i]->usercontext = reinterpret_cast<void*>(static_cast<intptr_t>(i));

        for (size_t pkt = 0; pkt < m_packetsPerUrb; ++pkt) {
            m_urbs[i]->iso_frame_desc[pkt].length = static_cast<unsigned int>(m_isoPacketSize);
            m_urbs[i]->iso_frame_desc[pkt].actual_length = 0;
            m_urbs[i]->iso_frame_desc[pkt].status = 0;
        }
    }

    m_urbsInitialized = true;
    m_totalSubmitted = 0;
    m_nextSubmitIndex = 0;

    LOGI("Initialized %d isochronous URBs: packetsPerUrb=%zu, bufferSize=%zu bytes, isoPacket=%zu", 
         NUM_URBS, m_packetsPerUrb, m_urbBufferSize, m_isoPacketSize);

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

    releaseUrbResources();
    
    // Disable audio streaming - set SPCMic Interface 3 back to alt setting 0
    // This stops the 84-channel audio streaming
    int streamingInterface = (m_streamInterfaceNumber >= 0) ? m_streamInterfaceNumber : 3;
    setInterface(streamingInterface, 0);
    
    LOGI("USB audio streaming stopped");
    return true;
}

bool USBAudioInterface::enableAudioStreaming() {
    LOGI("Enabling USB audio streaming for SPCMic device - following Linux USB audio driver sequence");
    int streamingInterface = (m_streamInterfaceNumber >= 0) ? m_streamInterfaceNumber : 3;
    int streamingAltSetting = (m_streamAltSetting >= 0) ? m_streamAltSetting : 1;
    uint8_t streamingEndpoint = (m_audioInEndpoint >= 0) ? static_cast<uint8_t>(m_audioInEndpoint) : 0x81;
    
    // STEP 1: Reset interface to alt 0 (disable streaming)
    // This is CRITICAL - Linux USB audio driver always does this first
    LOGI("Step 1: Setting Interface %d to alt 0 (disable streaming)", streamingInterface);
    setInterface(streamingInterface, 0);
    usleep(50000); // 50ms delay
    
    // STEP 2: Configure sample rate BEFORE enabling the interface
    // Linux USB audio does: usb_set_interface(alt 0) -> init_sample_rate() -> usb_set_interface(alt 1)
    LOGI("Step 2: Configuring sample rate to %d Hz on endpoint 0x%02x", m_sampleRate, streamingEndpoint);
    uint8_t sampleRateData[3];
    sampleRateData[0] = static_cast<uint8_t>(m_sampleRate & 0xFF);
    sampleRateData[1] = static_cast<uint8_t>((m_sampleRate >> 8) & 0xFF);
    sampleRateData[2] = static_cast<uint8_t>((m_sampleRate >> 16) & 0xFF);
    struct usbdevfs_ctrltransfer ctrl = {0};
    ctrl.bRequestType = 0x22; // Class, Endpoint, Host to Device
    ctrl.bRequest = 0x01;     // SET_CUR
    ctrl.wValue = 0x0100;     // CS_SAM_FREQ_CONTROL (Sampling Frequency Control)
    ctrl.wIndex = streamingEndpoint;       // Target audio endpoint
    ctrl.wLength = 3;         // 3 bytes for sample rate (24-bit)
    ctrl.timeout = 1000;
    ctrl.data = sampleRateData;
    
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
    pitch_ctrl.wIndex = streamingEndpoint;       // Target audio endpoint
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
    LOGI("Step 3: Setting Interface %d to alt %d (enable streaming)", streamingInterface, streamingAltSetting);
    struct usbdevfs_setinterface setintf = {0};
    setintf.interface = streamingInterface;
    setintf.altsetting = streamingAltSetting;
    
    int result = ioctl(m_deviceFd, USBDEVFS_SETINTERFACE, &setintf);
    if (result == 0) {
        LOGI("Successfully enabled Interface %d alt %d for streaming", streamingInterface, streamingAltSetting);
    } else {
        LOGE("Failed to set Interface %d alt %d: %s", streamingInterface, streamingAltSetting, strerror(errno));
        return false;
    }
    
    usleep(50000); // 50ms for device to start streaming
    
    m_isStreaming = true;
    LOGI("SPCMic streaming enabled - ready for isochronous transfers on endpoint 0x%02x", streamingEndpoint);
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
    
    // USB Audio Class: SPCMic uses isochronous transfers for real-time audio.
    // URB geometry is derived from the endpoint descriptors at runtime.

    if (!m_endpointInfoReady) {
        LOGE("Endpoint information not ready - cannot read audio data yet");
        return 0;
    }

    
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
        releaseUrbResources();
        m_totalSubmitted = 0;
        m_nextSubmitIndex = 0;
        m_frameNumberInitialized = false; // Reset frame number tracking
        m_wasStreaming = true;
        if (!ensureUrbResources()) {
            return 0;
        }
    } else if (!m_isStreaming && m_wasStreaming) {
        // Just stopped streaming
        LOGI("Streaming stopped - will re-initialize on next start");
        m_wasStreaming = false;
    }
    
    if (!m_urbsInitialized && !ensureUrbResources()) {
        return 0;
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
                LOGI("Submitted initial URB[%d] (%d/%d, %zu packets)", m_nextSubmitIndex, m_totalSubmitted, NUM_URBS, m_packetsPerUrb);
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
                releaseUrbResources();
                m_consecutiveSameUrbCount = 0;
                m_lastReapedUrbAddress = nullptr;
                m_stuckUrbDetected = false;
                
                LOGI("All URBs cancelled - will reinitialize on next readAudioData() call");
                return total_bytes_accumulated;
            }
            
            m_recentReapCheckpoint = m_reapCount;
        }
        
        // Collect data from all packets in this URB and check for errors
        size_t total_actual = 0;
        int error_count = 0;
        for (size_t pkt = 0; pkt < m_packetsPerUrb; ++pkt) {
            total_actual += completed_urb->iso_frame_desc[pkt].actual_length;
            if (completed_urb->iso_frame_desc[pkt].status != 0) {
                error_count++;
                // Log first few errors to understand what's happening
                if (m_reapCount <= 50 || (m_reapCount % 1000 == 0 && error_count <= 2)) {
                LOGE("URB[%d] packet[%zu] error: status=%d, actual=%u", 
                    urb_index, pkt, completed_urb->iso_frame_desc[pkt].status, 
                    completed_urb->iso_frame_desc[pkt].actual_length);
                }
            }
        }
        
        // Copy data to output buffer if we have room and data exists
        if (total_actual > 0 && total_bytes_accumulated < bufferSize) {
            uint8_t* urbData = static_cast<uint8_t*>(completed_urb->buffer);
            size_t packetOffset = 0;
            for (size_t pkt = 0; pkt < m_packetsPerUrb && total_bytes_accumulated < bufferSize; ++pkt) {
                unsigned int packetLength = completed_urb->iso_frame_desc[pkt].actual_length;
                if (packetLength > 0) {
                    if (packetOffset + packetLength > m_urbBufferSize) {
                        LOGE("Packet length %u exceeds URB buffer bounds (offset=%zu, size=%zu)", packetLength, packetOffset, m_urbBufferSize);
                        packetLength = std::min<unsigned int>(packetLength, static_cast<unsigned int>(m_urbBufferSize - packetOffset));
                    }
                    size_t bytesToCopy = std::min(static_cast<size_t>(packetLength), bufferSize - total_bytes_accumulated);
                    memcpy(buffer + total_bytes_accumulated, urbData + packetOffset, bytesToCopy);
                    total_bytes_accumulated += bytesToCopy;
                }
                packetOffset += m_isoPacketSize;
            }
            
            if (m_reapCount <= 20 || m_reapCount % 100 == 0) {
                size_t frameBytes = static_cast<size_t>(m_channelCount) * static_cast<size_t>(m_bytesPerSample);
                size_t samplesPerChannel = frameBytes > 0 ? total_actual / frameBytes : 0;
                LOGI("ISO URB[%d] reaped (reap#%d, loop#%d): %zu bytes (%zu samples/ch), accumulated=%zu", 
                     urb_index, m_reapCount, reap_loop, total_actual, samplesPerChannel, total_bytes_accumulated);
            }
        }
        
        // Re-submit this URB immediately for continuous streaming (kernel docs: "keep at least one URB queued")
        for (size_t pkt = 0; pkt < m_packetsPerUrb; ++pkt) {
            completed_urb->iso_frame_desc[pkt].actual_length = 0;
            completed_urb->iso_frame_desc[pkt].status = 0;
        }
        completed_urb->buffer_length = m_urbBufferSize;
        completed_urb->number_of_packets = static_cast<unsigned>(m_packetsPerUrb);
        
        int submit_result = ioctl(m_deviceFd, USBDEVFS_SUBMITURB, completed_urb);
        if (submit_result < 0) {
            LOGE("Failed to re-submit URB[%d]: %s (errno %d)", urb_index, strerror(errno), errno);
        } else if (m_reapCount <= 20) {
            LOGI("Re-submitted URB[%d] successfully", urb_index);
        }

        if (total_bytes_accumulated >= bufferSize) {
            break;
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

size_t USBAudioInterface::getRecommendedBufferSize() const {
    if (!m_endpointInfoReady || m_isoPacketSize == 0) {
        return 0;
    }

    size_t packetsPerService = std::max<size_t>(1, m_packetsPerServiceInterval);
    size_t targetPackets = packetsPerService * 8;
    size_t maxPackets = std::max<size_t>(1, MAX_URB_BUFFER_BYTES / m_isoPacketSize);
    size_t packetsPerUrb = std::max<size_t>(1, std::min(targetPackets, maxPackets));

    return m_isoPacketSize * packetsPerUrb;
}

void USBAudioInterface::release() {
    LOGI("Releasing USB audio interface");
    
    stopStreaming();
    
    releaseUrbResources();
    
    if (m_deviceFd >= 0) {
        int streamingInterface = (m_streamInterfaceNumber >= 0) ? m_streamInterfaceNumber : 3;
        LOGI("Set interface %d alt setting 0", streamingInterface);
        setInterface(streamingInterface, 0);
        m_deviceFd = -1;
    }
    
    LOGI("USB audio interface released");
}

#include "usb_audio_interface.h"
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
#include <limits>
#include <sstream>
#include <string>
#include <cstddef>

#define LOG_TAG "USBAudioInterface"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOG_FATAL_IF(cond, ...) \
    do { \
        if (cond) { \
            __android_log_assert(#cond, LOG_TAG, __VA_ARGS__); \
        } \
    } while (false)

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
#ifndef USB_DT_CS_INTERFACE
#define USB_DT_CS_INTERFACE 0x24
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
#ifndef USB_CLASS_AUDIO
#define USB_CLASS_AUDIO 0x01
#endif
#ifndef USB_SUBCLASS_AUDIOCONTROL
#define USB_SUBCLASS_AUDIOCONTROL 0x01
#endif
#ifndef USB_SUBCLASS_AUDIOSTREAMING
#define USB_SUBCLASS_AUDIOSTREAMING 0x02
#endif
#ifndef USBDEVFS_IOCTL
#define USBDEVFS_IOCTL _IOWR('U', 32, struct usbdevfs_ioctl)
#endif
#ifndef USBDEVFS_CLEAR_HALT
#define USBDEVFS_CLEAR_HALT _IO('U', 2)
#endif

// USB Audio Class request codes
constexpr uint8_t UAC_SET_CUR = 0x01;
constexpr uint8_t UAC_GET_CUR = 0x81;

// USB Audio Class control selectors
constexpr uint8_t UAC_SAMPLING_FREQ_CONTROL = 0x01;

// UAC2 Clock Source control selectors
constexpr uint8_t UAC2_CS_CONTROL_CLOCK_VALID = 0x01;
constexpr uint8_t UAC2_CS_CONTROL_SAM_FREQ = 0x00;

// UAC2 Clock Selector control selectors
constexpr uint8_t UAC2_CX_CLOCK_SELECTOR = 0x00;

// USB Audio Class descriptor subtypes
constexpr uint8_t UAC_CS_SUBTYPE_AS_GENERAL = 0x01;
constexpr uint8_t UAC_CS_SUBTYPE_FORMAT_TYPE = 0x02;
constexpr uint8_t UAC_CS_SUBTYPE_CLOCK_SOURCE = 0x0A;
constexpr uint8_t UAC_CS_SUBTYPE_CLOCK_SELECTOR = 0x0B;
constexpr uint8_t UAC_CS_SUBTYPE_CLOCK_MULTIPLIER = 0x0C;

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

inline uint32_t read_le32(const void* ptr) {
    const uint8_t* bytes = static_cast<const uint8_t*>(ptr);
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

} // namespace

bool USBAudioInterface::isControlReadable(uint32_t bmControls, uint8_t controlBitIndex) {
    const uint32_t shift = static_cast<uint32_t>(controlBitIndex) * 2u;
    const uint32_t field = (bmControls >> shift) & 0x3u;
    return field == 0x1u || field == 0x3u;
}

bool USBAudioInterface::isControlWritable(uint32_t bmControls, uint8_t controlBitIndex) {
    const uint32_t shift = static_cast<uint32_t>(controlBitIndex) * 2u;
    const uint32_t field = (bmControls >> shift) & 0x3u;
    return field == 0x2u || field == 0x3u;
}

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
    , m_pendingData()
    , m_pendingReadOffset(0)
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
    , m_isSuperSpeed(false)
    , m_effectiveSampleRate(48000.0)
    , m_controlInterfaceNumber(-1)
    , m_clockSourceId(-1)
    , m_clockFrequencyProgrammable(false)
    , m_streamClockEntityId(-1)
    , m_clockSelectorId(-1)
    , m_clockSelectorInputs()
    , m_clockSelectorControls(0)
    , m_clockMultiplierId(-1)
    , m_clockMultiplierControls(0)
    , m_clockSources()
    , m_supportedSampleRates()
    , m_supportsContinuousSampleRate(false)
    , m_minContinuousSampleRate(0)
    , m_maxContinuousSampleRate(0) {
}

USBAudioInterface::~USBAudioInterface() {
    release();
}

const USBAudioInterface::ClockSourceDetails* USBAudioInterface::findClockSourceDetails(uint8_t id) const {
    auto it = m_clockSourceMap.find(id);
    if (it != m_clockSourceMap.end()) {
        return &it->second;
    }
    return nullptr;
}

const USBAudioInterface::ClockSelectorDetails* USBAudioInterface::findClockSelectorDetails(uint8_t id) const {
    auto it = m_clockSelectorMap.find(id);
    if (it != m_clockSelectorMap.end()) {
        return &it->second;
    }
    return nullptr;
}

const USBAudioInterface::ClockMultiplierDetails* USBAudioInterface::findClockMultiplierDetails(uint8_t id) const {
    auto it = m_clockMultiplierMap.find(id);
    if (it != m_clockMultiplierMap.end()) {
        return &it->second;
    }
    return nullptr;
}

bool USBAudioInterface::initialize(int deviceFd, int sampleRate, int channelCount) {
    LOGI("Initializing USB audio interface: fd=%d, rate=%d, channels=%d", 
         deviceFd, sampleRate, channelCount);
    
    m_deviceFd = deviceFd;
    m_sampleRate = sampleRate;
    m_channelCount = channelCount;
    m_supportedSampleRates.clear();
    m_supportsContinuousSampleRate = false;
    m_minContinuousSampleRate = 0;
    m_maxContinuousSampleRate = 0;
    
    if (m_deviceFd < 0) {
        LOGE("Invalid device file descriptor");
        return false;
    }
    
    // Find the correct audio input endpoint
    if (!findAudioEndpoint()) {
        LOGE("Failed to find audio input endpoint");
        return false;
    }
    
    int derivedRate = getEffectiveSampleRateRounded();
    if (derivedRate > 0 && std::abs(derivedRate - m_sampleRate) > 1) {
        LOGI("Descriptor-derived effective rate is approximately %d Hz for selected endpoint (requested %d Hz)",
             derivedRate, m_sampleRate);
    }

    // Ensure the isochronous pipe is idle before upper layers begin streaming.
    if (!flushIsochronousEndpoint()) {
        LOGE("Isochronous endpoint flush reported issues; continuing with best effort state");
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

bool USBAudioInterface::setInterfaceWithRetry(int interfaceNum, int altSetting, int maxRetries) {
    LOGI("Setting interface %d to alt %d with retry (maxRetries=%d)", 
         interfaceNum, altSetting, maxRetries);

    for (int retry = 0; retry < maxRetries; retry++) {
        struct usbdevfs_setinterface setintf;
        setintf.interface = interfaceNum;
        setintf.altsetting = altSetting;
        
        int result = ioctl(m_deviceFd, USBDEVFS_SETINTERFACE, &setintf);
        if (result == 0) {
            LOGI("Successfully set interface %d alt %d (attempt %d/%d)", 
                 interfaceNum, altSetting, retry + 1, maxRetries);
            return true;
        }

        int err = errno;
        
        // EPROTO errors can occur during interface transitions - retry with backoff
        if (err == EPROTO && retry < maxRetries - 1) {
            // Exponential backoff: 5ms, 10ms, 20ms, 40ms, 80ms
            int delayUs = 5000 * (1 << retry);
            LOGI("Interface setting failed with EPROTO (attempt %d/%d), retrying after %d ms", 
                 retry + 1, maxRetries, delayUs / 1000);
            usleep(delayUs);
            continue;
        }

        // Other errors or final retry - fail
        LOGE("Failed to set interface %d alt %d: result=%d errno=%d (%s) (attempt %d/%d)",
             interfaceNum, altSetting, result, err, strerror(err), retry + 1, maxRetries);
        
        if (retry < maxRetries - 1) {
            int delayUs = 5000 * (1 << retry);
            LOGI("Retrying after %d ms", delayUs / 1000);
            usleep(delayUs);
        }
    }

    LOGE("Failed to set interface %d alt %d after %d retries", 
         interfaceNum, altSetting, maxRetries);
    return false;
}

bool USBAudioInterface::configureSampleRate(int sampleRate) {
    LOGI("Configuring sample rate to %d Hz", sampleRate);

    uint8_t sampleRateData[4];
    sampleRateData[0] = sampleRate & 0xFF;
    sampleRateData[1] = (sampleRate >> 8) & 0xFF;
    sampleRateData[2] = (sampleRate >> 16) & 0xFF;
    sampleRateData[3] = (sampleRate >> 24) & 0xFF;

    resolveAndApplyClockSelection(true);

    const ClockSourceDetails* clockDetails = findClockSourceDetails(static_cast<uint8_t>(m_clockSourceId));
    bool clockValidReadable = clockDetails && isControlReadable(clockDetails->bmControls, UAC2_CS_CONTROL_CLOCK_VALID);
    bool clockFreqReadable = clockDetails && isControlReadable(clockDetails->bmControls, UAC2_CS_CONTROL_SAM_FREQ);
    bool clockFreqWritable = clockDetails && isControlWritable(clockDetails->bmControls, UAC2_CS_CONTROL_SAM_FREQ);

    if (clockDetails && clockValidReadable) {
        if (!evaluateClockValidity(static_cast<uint8_t>(m_clockSourceId), clockDetails, 20)) {
            LOGI("Clock source %d validity not yet confirmed; attempting alternate inputs", m_clockSourceId);
            if (resolveAndApplyClockSelection(false)) {
                clockDetails = findClockSourceDetails(static_cast<uint8_t>(m_clockSourceId));
                clockFreqReadable = clockDetails && isControlReadable(clockDetails->bmControls, UAC2_CS_CONTROL_SAM_FREQ);
                clockFreqWritable = clockDetails && isControlWritable(clockDetails->bmControls, UAC2_CS_CONTROL_SAM_FREQ);
                clockValidReadable = clockDetails && isControlReadable(clockDetails->bmControls, UAC2_CS_CONTROL_CLOCK_VALID);
            }
        }
    } else if (clockDetails) {
        LOGI("Clock source %d does not expose CLOCK_VALID control; proceeding without validation", m_clockSourceId);
    }

    if (clockDetails && !clockFreqReadable) {
        LOGI("Clock source %d does not expose a readable sample-rate control; verification will rely on endpoint reports",
             m_clockSourceId);
    }

    bool attemptedClock = false;
    bool clockSuccess = false;

    auto ensureSampleRateTracked = [&](uint32_t rate) {
        if (rate == 0) {
            return;
        }
        if (std::find(m_supportedSampleRates.begin(), m_supportedSampleRates.end(), rate) == m_supportedSampleRates.end()) {
            m_supportedSampleRates.push_back(rate);
        }
    };

    auto updateEffectiveOnSuccess = [&](const char* source) {
        m_sampleRate = sampleRate;
        m_effectiveSampleRate = static_cast<double>(sampleRate);
        ensureSampleRateTracked(static_cast<uint32_t>(sampleRate));
        LOGI("Sample rate %d Hz accepted via %s", sampleRate, source);
    };

    if (m_deviceFd >= 0 && m_clockSourceId >= 0 && clockFreqWritable) {
        struct ClockSetAttempt {
            uint16_t wIndex;
            uint16_t wLength;
            const char* description;
        };

        std::vector<ClockSetAttempt> attempts;
        attempts.reserve(6);

        auto makeIndex = [&](int interfaceNumber) -> uint16_t {
            uint16_t high = static_cast<uint16_t>(m_clockSourceId) << 8;
            uint16_t low = (interfaceNumber >= 0) ? static_cast<uint16_t>(interfaceNumber & 0xFF)
                                                  : static_cast<uint16_t>(0x00);
            return static_cast<uint16_t>(high | low);
        };

        auto addAttempt = [&](int interfaceNumber, uint16_t length, const char* description) {
            ClockSetAttempt attempt{makeIndex(interfaceNumber), length, description};
            auto duplicate = std::find_if(attempts.begin(), attempts.end(), [&](const ClockSetAttempt& entry) {
                return entry.wIndex == attempt.wIndex && entry.wLength == attempt.wLength;
            });
            if (duplicate == attempts.end()) {
                attempts.push_back(attempt);
            }
        };

        if (m_controlInterfaceNumber >= 0) {
            addAttempt(m_controlInterfaceNumber, 4, "clock source 32-bit (audio control interface)");
            addAttempt(m_controlInterfaceNumber, 3, "clock source 24-bit (audio control interface)");
        }

        if (m_streamInterfaceNumber >= 0) {
            addAttempt(m_streamInterfaceNumber, 4, "clock source 32-bit (audio streaming interface)");
            addAttempt(m_streamInterfaceNumber, 3, "clock source 24-bit (audio streaming interface)");
        }

        addAttempt(-1, 4, "clock source 32-bit (entity only)");
        addAttempt(-1, 3, "clock source 24-bit (entity only)");

        attemptedClock = !attempts.empty();

        for (const auto& attempt : attempts) {
            LOGI("Attempting %s: wIndex=0x%04x wLength=%u", attempt.description, attempt.wIndex, attempt.wLength);

            struct usbdevfs_ctrltransfer ctrl = {};
            ctrl.bRequestType = 0x21; // Class, Interface, Host to Device
            ctrl.bRequest = UAC_SET_CUR;
            ctrl.wValue = (UAC_SAMPLING_FREQ_CONTROL << 8) | 0x00;
            ctrl.wIndex = attempt.wIndex;
            ctrl.wLength = attempt.wLength;
            ctrl.timeout = 1000;
            ctrl.data = sampleRateData;

            int result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
            if (result >= 0) {
                LOGI("Clock source SET_CUR succeeded using %s", attempt.description);
                updateEffectiveOnSuccess("clock source");
                clockSuccess = true;
                break;
            }

            int err = errno;
            LOGE("Clock source attempt failed (%s): result=%d errno=%d %s",
                 attempt.description, result, err, strerror(err));

            if (err == EBUSY) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    } else if (m_clockSourceId >= 0 && clockDetails) {
        LOGI("Clock source %d does not allow host sample-rate programming (bmControls=0x%08x)",
             m_clockSourceId, clockDetails->bmControls);
    }
    m_clockFrequencyProgrammable = clockFreqWritable;

    bool endpointSuccess = false;

    if (!clockSuccess) {
        struct usbdevfs_ctrltransfer ctrl = {};
        ctrl.bRequestType = 0x22; // Class, Endpoint, Host to Device
        ctrl.bRequest = UAC_SET_CUR;
        ctrl.wValue = (UAC_SAMPLING_FREQ_CONTROL << 8) | 0x00;
        ctrl.wIndex = m_audioInEndpoint; // Target the audio input endpoint
        ctrl.wLength = 3; // 24-bit sample rate
        ctrl.timeout = 1000;
        ctrl.data = sampleRateData;

        LOGI("Attempting endpoint SET_CUR fallback: endpoint=0x%02x targetRate=%d Hz", m_audioInEndpoint, sampleRate);
        int result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
        if (result >= 0) {
            LOGI("Sample rate configured via endpoint control (UAC 1.0 fallback)");
            updateEffectiveOnSuccess("endpoint fallback");
            endpointSuccess = true;
        } else {
            int err = errno;
            if (attemptedClock) {
                LOGE("Endpoint fallback also failed after clock source attempts (result=%d errno=%d %s)",
                     result, err, strerror(err));
            } else {
                LOGE("Failed to configure sample rate via endpoint control (result=%d errno=%d %s)",
                     result, err, strerror(err));
            }

            LOGI("Assuming sample rate is set by alternate setting selection");
        }
    }

    uint32_t reportedRate = 0;
    const char* reportedSource = nullptr;
    if (queryCurrentSampleRate(reportedRate, &reportedSource)) {
        LOGI("Device-reported current sample rate via %s: %u Hz",
             reportedSource ? reportedSource : "unknown", reportedRate);
        m_effectiveSampleRate = static_cast<double>(reportedRate);
        return true;
    }

    return clockSuccess || endpointSuccess;
}

bool USBAudioInterface::readSampleRateFromClock(uint32_t& outRate) {
    if (m_deviceFd < 0 || m_clockSourceId < 0) {
        return false;
    }

    const ClockSourceDetails* details = findClockSourceDetails(static_cast<uint8_t>(m_clockSourceId));
    if (!details || !isControlReadable(details->bmControls, UAC2_CS_CONTROL_SAM_FREQ)) {
        return false;
    }

    uint8_t buffer[4] = {0};
    struct usbdevfs_ctrltransfer ctrl = {};
    ctrl.bRequestType = 0xA1; // Class, Interface, Device to Host
    ctrl.bRequest = UAC_GET_CUR;
    ctrl.wValue = (UAC_SAMPLING_FREQ_CONTROL << 8) | 0x00;
    ctrl.timeout = 1000;
    ctrl.data = buffer;

    std::vector<int> interfaceCandidates;
    if (m_controlInterfaceNumber >= 0) {
        interfaceCandidates.push_back(m_controlInterfaceNumber);
    }
    if (m_streamInterfaceNumber >= 0 &&
        (m_controlInterfaceNumber < 0 || m_streamInterfaceNumber != m_controlInterfaceNumber)) {
        interfaceCandidates.push_back(m_streamInterfaceNumber);
    }
    if (interfaceCandidates.empty()) {
        interfaceCandidates.push_back(-1);
    }

    const uint16_t lengths[] = {4, 3};
    for (int interfaceNumber : interfaceCandidates) {
        ctrl.wIndex = static_cast<uint16_t>((m_clockSourceId << 8) |
                                            ((interfaceNumber >= 0) ? (interfaceNumber & 0xFF) : 0x00));

        for (uint16_t length : lengths) {
            ctrl.wLength = length;
            int result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
            if (result >= 0) {
                if (length == 4) {
                    outRate = read_le32(buffer);
                } else {
                    outRate = static_cast<uint32_t>(buffer[0]) |
                              (static_cast<uint32_t>(buffer[1]) << 8) |
                              (static_cast<uint32_t>(buffer[2]) << 16);
                }
                LOGI("Clock source GET_CUR returned %u Hz (entity=%d interface=%d length=%u)",
                     outRate, m_clockSourceId, interfaceNumber, length);
                return true;
            }

            int err = errno;
            LOGI("Clock source GET_CUR attempt failed (interface=%d length=%u): errno=%d %s",
                 interfaceNumber, length, err, strerror(err));
        }
    }

    return false;
}

bool USBAudioInterface::readSampleRateFromEndpoint(uint32_t& outRate) {
    if (m_deviceFd < 0 || m_audioInEndpoint < 0) {
        return false;
    }

    uint8_t buffer[3] = {0};
    struct usbdevfs_ctrltransfer ctrl = {};
    ctrl.bRequestType = 0xA2; // Class, Endpoint, Device to Host
    ctrl.bRequest = UAC_GET_CUR;
    ctrl.wValue = (UAC_SAMPLING_FREQ_CONTROL << 8) | 0x00;
    ctrl.wIndex = static_cast<uint16_t>(m_audioInEndpoint & 0xFF);
    ctrl.wLength = sizeof(buffer);
    ctrl.timeout = 1000;
    ctrl.data = buffer;

    int result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
    if (result >= 0) {
        outRate = static_cast<uint32_t>(buffer[0]) |
                  (static_cast<uint32_t>(buffer[1]) << 8) |
                  (static_cast<uint32_t>(buffer[2]) << 16);
        LOGI("Endpoint GET_CUR returned %u Hz (endpoint=0x%02x)", outRate, m_audioInEndpoint);
        return true;
    }

    int err = errno;
    LOGI("Endpoint GET_CUR failed: errno=%d %s", err, strerror(err));
    return false;
}

bool USBAudioInterface::queryCurrentSampleRate(uint32_t& outRate, const char** sourceName) {
    if (sourceName) {
        *sourceName = nullptr;
    }

    if (readSampleRateFromClock(outRate)) {
        if (sourceName) {
            *sourceName = "clock source";
        }
        return true;
    }

    if (readSampleRateFromEndpoint(outRate)) {
        if (sourceName) {
            *sourceName = "endpoint";
        }
        return true;
    }

    return false;
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
    m_controlInterfaceNumber = -1;
    m_clockSourceId = -1;
    m_clockFrequencyProgrammable = false;
    m_streamClockEntityId = -1;
    m_clockSelectorId = -1;
    m_clockSelectorInputs.clear();
    m_clockSelectorControls = 0;
    m_clockMultiplierId = -1;
    m_clockMultiplierControls = 0;
    m_clockSources.clear();
    m_clockSourceMap.clear();
    m_clockSelectorMap.clear();
    m_clockMultiplierMap.clear();
    m_supportedSampleRates.clear();
    m_supportsContinuousSampleRate = false;
    m_minContinuousSampleRate = 0;
    m_maxContinuousSampleRate = 0;

    const int requestedSampleRate = m_sampleRate;

    auto addSupportedRate = [&](uint32_t rate) {
        if (rate == 0) {
            return;
        }
        if (std::find(m_supportedSampleRates.begin(), m_supportedSampleRates.end(), rate) == m_supportedSampleRates.end()) {
            m_supportedSampleRates.push_back(rate);
        }
    };

    struct EndpointSelection {
        bool valid = false;
        int interfaceNumber = -1;
        int altSetting = -1;
        uint8_t endpointAddress = 0;
        size_t isoPacketSize = 0;
        size_t bytesPerInterval = 0;
        size_t packetsPerServiceInterval = 1;
        bool isSuperSpeed = false;
        bool isHighSpeed = false;
        bool supportsRequestedRate = false;
        uint32_t matchedSampleRate = 0;
        uint32_t preferredSampleRate = 0;
        double derivedSampleRate = 0.0;
        bool hasDerivedSampleRate = false;
    };

    EndpointSelection best;
    EndpointSelection current;
    bool inCandidateInterface = false;
    uint8_t currentInterfaceClass = 0;
    uint8_t currentInterfaceSubClass = 0;

    auto rateForComparison = [&](const EndpointSelection& entry) -> double {
        if (entry.hasDerivedSampleRate) {
            return entry.derivedSampleRate;
        }
        if (entry.matchedSampleRate > 0) {
            return static_cast<double>(entry.matchedSampleRate);
        }
        if (entry.preferredSampleRate > 0) {
            return static_cast<double>(entry.preferredSampleRate);
        }
        return 0.0;
    };

    auto diffFromRequested = [&](const EndpointSelection& entry) -> double {
        if (requestedSampleRate <= 0) {
            return std::numeric_limits<double>::infinity();
        }
        double comparisonRate = rateForComparison(entry);
        if (comparisonRate <= 0.0) {
            return std::numeric_limits<double>::infinity();
        }
        return std::fabs(comparisonRate - static_cast<double>(requestedSampleRate));
    };

    auto evaluateCurrent = [&](const EndpointSelection& candidate) {
        if (!candidate.valid) {
            return;
        }

        bool preferCandidate = false;

        if (!best.valid) {
            preferCandidate = true;
        } else {
            double candidateDiff = diffFromRequested(candidate);
            double bestDiff = diffFromRequested(best);

            double requestedRateAsDouble = static_cast<double>(requestedSampleRate);
            double tolerance = (requestedRateAsDouble > 0.0) ? requestedRateAsDouble * 0.05 : 0.0;
            bool candidateClose = std::isfinite(candidateDiff) && candidateDiff <= tolerance;
            bool bestClose = std::isfinite(bestDiff) && bestDiff <= tolerance;

            if (candidate.supportsRequestedRate && !best.supportsRequestedRate) {
                preferCandidate = true;
            } else if (candidate.supportsRequestedRate == best.supportsRequestedRate) {
                if (requestedSampleRate > 0 && (candidateClose || bestClose)) {
                    if (candidateClose && !bestClose) {
                        preferCandidate = true;
                    } else if (candidateClose == bestClose) {
                        if (candidateDiff + 1.0 < bestDiff) {
                            preferCandidate = true;
                        } else if (std::fabs(candidateDiff - bestDiff) <= 1.0 &&
                                   candidate.bytesPerInterval < best.bytesPerInterval) {
                            preferCandidate = true;
                        }
                    }
                } else if (requestedSampleRate > 0 && std::isfinite(candidateDiff) && std::isfinite(bestDiff)) {
                    if (candidateDiff + 1.0 < bestDiff) {
                        preferCandidate = true;
                    } else if (std::fabs(candidateDiff - bestDiff) <= 1.0 &&
                               candidate.bytesPerInterval < best.bytesPerInterval) {
                        preferCandidate = true;
                    }
                } else {
                    if (candidate.bytesPerInterval < best.bytesPerInterval) {
                        preferCandidate = true;
                    }
                }
            }
        }

        if (preferCandidate) {
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
                currentInterfaceClass = intf->bInterfaceClass;
                currentInterfaceSubClass = intf->bInterfaceSubClass;
                if (currentInterfaceClass == USB_CLASS_AUDIO &&
                    currentInterfaceSubClass == USB_SUBCLASS_AUDIOCONTROL &&
                    m_controlInterfaceNumber < 0) {
                    m_controlInterfaceNumber = intf->bInterfaceNumber;
                    LOGI("Detected AudioControl interface: %d", m_controlInterfaceNumber);
                }

                bool isAudioStreaming = (currentInterfaceClass == USB_CLASS_AUDIO &&
                                         currentInterfaceSubClass == USB_SUBCLASS_AUDIOSTREAMING);
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

            case USB_DT_CS_INTERFACE: {
                const uint8_t* body = &descriptor[offset];
                if (header->bLength < 3) {
                    break;
                }
                uint8_t subType = body[2];

                if (currentInterfaceClass == USB_CLASS_AUDIO) {
                    if (currentInterfaceSubClass == USB_SUBCLASS_AUDIOCONTROL) {
                        if (subType == UAC_CS_SUBTYPE_CLOCK_SOURCE && header->bLength >= 8) {
                            uint8_t clockId = body[3];
                            uint8_t bmAttributes = body[4];
                            uint32_t bmControls = 0;
                            for (size_t idx = 0; idx < 4 && (5 + idx) < header->bLength; ++idx) {
                                bmControls |= static_cast<uint32_t>(body[5 + idx]) << (idx * 8);
                            }
                            bool frequencyProgrammable = isControlWritable(bmControls, UAC2_CS_CONTROL_SAM_FREQ);
                            if (clockId != 0) {
                                ClockSourceDetails details;
                                details.id = clockId;
                                details.attributes = bmAttributes;
                                details.bmControls = bmControls;
                                m_clockSourceMap[clockId] = details;
                                m_clockSources.push_back(details);
                                if (m_clockSourceId < 0) {
                                    m_clockSourceId = clockId;
                                }
                                LOGI("Found Clock Source descriptor: id=%u bmAttributes=0x%02x bmControls=0x%08x programmable=%d",
                                     clockId, bmAttributes, bmControls, frequencyProgrammable ? 1 : 0);
                            }
                        } else if (subType == UAC_CS_SUBTYPE_CLOCK_SELECTOR && header->bLength >= 7) {
                            uint8_t selectorId = body[3];
                            uint8_t numInputs = body[4];
                            size_t minLength = static_cast<size_t>(7) + numInputs;
                            if (selectorId != 0 && header->bLength >= minLength) {
                                std::vector<uint8_t> inputs;
                                inputs.reserve(numInputs);
                                for (uint8_t idx = 0; idx < numInputs; ++idx) {
                                    inputs.push_back(body[5 + idx]);
                                }
                                size_t controlOffset = 5 + numInputs;
                                uint32_t selectorControls = 0;
                                if (header->bLength > controlOffset) {
                                    selectorControls = body[controlOffset];
                                }
                                if (header->bLength > controlOffset + 1) {
                                    selectorControls |= static_cast<uint32_t>(body[controlOffset + 1]) << 8;
                                }
                                if (header->bLength > controlOffset + 2) {
                                    selectorControls |= static_cast<uint32_t>(body[controlOffset + 2]) << 16;
                                }
                                if (header->bLength > controlOffset + 3) {
                                    selectorControls |= static_cast<uint32_t>(body[controlOffset + 3]) << 24;
                                }
                                uint8_t selectorString = (header->bLength > controlOffset + 1) ? body[controlOffset + 1] : 0;
                                std::ostringstream oss;
                                for (size_t idx = 0; idx < inputs.size(); ++idx) {
                                    if (idx > 0) {
                                        oss << ",";
                                    }
                                    oss << static_cast<int>(inputs[idx]);
                                }
                                std::string inputsStr = oss.str();
                                LOGI("Found Clock Selector descriptor: id=%u numInputs=%u inputs=[%s] bmControls=0x%08x iSelector=%u",
                                     selectorId, numInputs, inputsStr.c_str(), selectorControls, selectorString);
                                ClockSelectorDetails selectorDetails;
                                selectorDetails.id = selectorId;
                                selectorDetails.inputs = inputs;
                                selectorDetails.bmControls = selectorControls;
                                m_clockSelectorMap[selectorId] = selectorDetails;
                                if (m_clockSelectorId < 0) {
                                    m_clockSelectorId = selectorId;
                                    m_clockSelectorInputs = inputs;
                                    m_clockSelectorControls = static_cast<uint8_t>(selectorControls & 0xFF);
                                }
                            }
                        } else if (subType == UAC_CS_SUBTYPE_CLOCK_MULTIPLIER && header->bLength >= 7) {
                            uint8_t multiplierId = body[3];
                            uint8_t sourceId = body[4];
                            uint8_t multiplierControls = body[5];
                            uint8_t multiplierString = (header->bLength > 6) ? body[6] : 0;
                            LOGI("Found Clock Multiplier descriptor: id=%u sourceId=%u bmControls=0x%02x iMultiplier=%u",
                                 multiplierId, sourceId, multiplierControls, multiplierString);
                            if (m_clockMultiplierId < 0) {
                                m_clockMultiplierId = multiplierId;
                                m_clockMultiplierControls = multiplierControls;
                            }
                            ClockMultiplierDetails multiplierDetails;
                            multiplierDetails.id = multiplierId;
                            multiplierDetails.sourceId = sourceId;
                            m_clockMultiplierMap[multiplierId] = multiplierDetails;
                        }
                    } else if (currentInterfaceSubClass == USB_SUBCLASS_AUDIOSTREAMING && inCandidateInterface) {
                        if (subType == UAC_CS_SUBTYPE_AS_GENERAL) {
                            if (header->bLength >= 8) {
                                uint8_t clockId = body[7];
                                if (clockId != 0) {
                                    m_streamClockEntityId = clockId;
                                    LOGI("Streaming interface references clock entity id=%u", clockId);
                                }
                            }
                        } else if (subType == UAC_CS_SUBTYPE_FORMAT_TYPE) {
                            if (header->bLength >= 8) {
                                uint8_t formatType = body[3];
                                uint8_t samFreqType = body[7];
                                size_t freqListBytes = (header->bLength > 8) ? (header->bLength - 8) : 0;
                                const uint8_t* freqPtr = body + 8;
                                LOGI("AudioStreaming format descriptor: formatType=%u samFreqType=%u freqBytes=%zu", formatType, samFreqType, freqListBytes);
                                auto readFreq = [&](const uint8_t* ptr, size_t availableBytes) -> uint32_t {
                                    if (availableBytes >= 4) {
                                        return read_le32(ptr);
                                    } else if (availableBytes >= 3) {
                                        return static_cast<uint32_t>(ptr[0]) |
                                               (static_cast<uint32_t>(ptr[1]) << 8) |
                                               (static_cast<uint32_t>(ptr[2]) << 16);
                                    }
                                    return 0;
                                };

                                if (samFreqType == 0) {
                                    // Continuous range; treat as supporting requested rate
                                    bool hasRequest = requestedSampleRate > 0;
                                    current.supportsRequestedRate = hasRequest;
                                    current.matchedSampleRate = hasRequest ? static_cast<uint32_t>(requestedSampleRate) : 0;
                                    current.preferredSampleRate = hasRequest ? static_cast<uint32_t>(requestedSampleRate) : 0;
                                    m_clockFrequencyProgrammable = true;
                                    m_supportsContinuousSampleRate = true;
                                    if (freqListBytes >= 6) {
                                        uint32_t minFreq = readFreq(freqPtr, freqListBytes);
                                        uint32_t maxFreq = readFreq(freqPtr + 3, freqListBytes - 3);
                                        m_minContinuousSampleRate = minFreq;
                                        m_maxContinuousSampleRate = maxFreq;
                                        LOGI("AudioStreaming continuous frequency range: %u-%u Hz", minFreq, maxFreq);
                                        addSupportedRate(minFreq);
                                        addSupportedRate(maxFreq);
                                    } else {
                                        LOGI("AudioStreaming continuous frequency range advertised (bytes=%zu)", freqListBytes);
                                    }
                                } else if (samFreqType > 0) {
                                    size_t perEntryBytes = (samFreqType > 0) ? (freqListBytes / samFreqType) : 0;
                                    if (perEntryBytes == 0) {
                                        perEntryBytes = 3;
                                    }
                                    for (uint8_t idx = 0; idx < samFreqType; ++idx) {
                                        size_t entryOffset = static_cast<size_t>(idx) * perEntryBytes;
                                        if (entryOffset >= freqListBytes) {
                                            break;
                                        }
                                        size_t remaining = freqListBytes - entryOffset;
                                        uint32_t freq = readFreq(freqPtr + entryOffset, remaining);
                                        if (freq == 0) {
                                            continue;
                                        }
                                        LOGI("AudioStreaming discrete frequency[%u]=%u Hz", idx, freq);
                                        addSupportedRate(freq);
                                        if (idx == 0 && current.preferredSampleRate == 0) {
                                            current.preferredSampleRate = freq;
                                        }
                                        if (requestedSampleRate > 0 && freq == static_cast<uint32_t>(requestedSampleRate)) {
                                            current.supportsRequestedRate = true;
                                            current.matchedSampleRate = freq;
                                        }
                                    }
                                    if (!current.supportsRequestedRate && current.preferredSampleRate == 0 && freqListBytes >= perEntryBytes) {
                                        current.preferredSampleRate = readFreq(freqPtr, freqListBytes);
                                    }
                                }

                                (void)formatType; // Currently unused but parsed for completeness
                            }
                        }
                    }
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
                candidate.isHighSpeed = (transactionsPerService > 1) || (payloadPerInterval > 1023);

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
                        candidate.isHighSpeed = false;
                    }
                }

                double frameBytes = static_cast<double>(m_channelCount) * static_cast<double>(m_bytesPerSample);
                if (candidate.valid && frameBytes > 0.0) {
                    size_t intervalFactor = std::max<size_t>(1, candidate.packetsPerServiceInterval);
                    double baseRate = candidate.isSuperSpeed ? 8000.0 : (candidate.isHighSpeed ? 8000.0 : 1000.0);
                    double framesPerInterval = static_cast<double>(candidate.isoPacketSize) / frameBytes;
                    candidate.derivedSampleRate = framesPerInterval * (baseRate / static_cast<double>(intervalFactor));
                    candidate.hasDerivedSampleRate = candidate.derivedSampleRate > 0.0;
                }

                if (candidate.valid) {
                    if (candidate.hasDerivedSampleRate) {
                        LOGI("Found candidate endpoint 0x%02x (interface %d alt %d): basePacket=%d, transactions=%d, bytesPerInterval=%zu, derivedRate=%.2f Hz", 
                             candidate.endpointAddress, candidate.interfaceNumber, candidate.altSetting,
                             basePacketSize, transactionsPerService, candidate.bytesPerInterval, candidate.derivedSampleRate);
                    } else {
                        LOGI("Found candidate endpoint 0x%02x (interface %d alt %d): basePacket=%d, transactions=%d, bytesPerInterval=%zu", 
                             candidate.endpointAddress, candidate.interfaceNumber, candidate.altSetting,
                             basePacketSize, transactionsPerService, candidate.bytesPerInterval);
                    }
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
    m_isHighSpeed = best.isHighSpeed;
    m_endpointInfoReady = true;

    LOGI("Selected endpoint 0x%02x: isoPacketSize=%zu, bytesPerInterval=%zu, packetsPerServiceInterval=%zu, superSpeed=%d", 
         m_audioInEndpoint, m_isoPacketSize, m_bytesPerInterval, m_packetsPerServiceInterval, m_isSuperSpeed ? 1 : 0);

    if (best.matchedSampleRate > 0 && requestedSampleRate <= 0) {
        LOGI("Adopting descriptor-matched discrete rate %u Hz (no explicit request)", best.matchedSampleRate);
        m_sampleRate = static_cast<int>(best.matchedSampleRate);
    } else if (!best.supportsRequestedRate && best.preferredSampleRate > 0) {
        if (requestedSampleRate <= 0) {
            LOGI("Using preferred descriptor rate %u Hz (no explicit request)", best.preferredSampleRate);
            m_sampleRate = static_cast<int>(best.preferredSampleRate);
        } else {
            LOGI("Descriptor does not list requested %d Hz; keeping explicit request and will attempt to program device", requestedSampleRate);
        }
    }

    if (!resolveAndApplyClockSelection(true)) {
        if (resolveAndApplyClockSelection(false)) {
            LOGI("Clock topology resolved without validation; proceeding with best-effort selection (id=%d)", m_clockSourceId);
        } else if (m_clockSourceId >= 0) {
            LOGI("Using descriptor-provided clock source id=%d (validation unavailable)", m_clockSourceId);
        } else {
            LOGI("No clock source could be resolved from descriptors");
        }
    } else {
        LOGI("Clock topology resolved successfully; active clock source id=%d", m_clockSourceId);
    }

    if (m_clockSourceId >= 0) {
        const ClockSourceDetails* sourceDetails = findClockSourceDetails(static_cast<uint8_t>(m_clockSourceId));
        if (sourceDetails) {
            m_clockFrequencyProgrammable = isControlWritable(sourceDetails->bmControls, UAC2_CS_CONTROL_SAM_FREQ);
            LOGI("Clock source detected: id=%d programmable=%d (bmAttributes=0x%02x bmControls=0x%08x)",
                 m_clockSourceId, m_clockFrequencyProgrammable ? 1 : 0, sourceDetails->attributes, sourceDetails->bmControls);
        } else {
            LOGI("Clock source detected: id=%d (details not found in parsed list)", m_clockSourceId);
        }
    }

    addSupportedRate(static_cast<uint32_t>(m_sampleRate));
    if (best.hasDerivedSampleRate) {
        addSupportedRate(static_cast<uint32_t>(std::lround(best.derivedSampleRate)));
    }
    if (best.matchedSampleRate > 0) {
        addSupportedRate(best.matchedSampleRate);
    }

    if (!m_supportedSampleRates.empty()) {
        std::sort(m_supportedSampleRates.begin(), m_supportedSampleRates.end());
    }

    updateEffectiveSampleRate();

    return true;
}

bool USBAudioInterface::getClockSelectorValue(uint8_t selectorId, uint8_t& outPin) const {
    if (m_deviceFd < 0 || m_controlInterfaceNumber < 0) {
        return false;
    }

    uint8_t value = 0;
    struct usbdevfs_ctrltransfer ctrl = {};
    ctrl.bRequestType = 0xA1; // Class, Interface, Device to Host
    ctrl.bRequest = UAC_GET_CUR;
    ctrl.wValue = static_cast<uint16_t>(UAC2_CX_CLOCK_SELECTOR) << 8;
    ctrl.wIndex = (static_cast<uint16_t>(selectorId) << 8) |
                  static_cast<uint16_t>(m_controlInterfaceNumber & 0xFF);
    ctrl.wLength = 1;
    ctrl.timeout = 1000;
    ctrl.data = &value;

    int result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
    if (result >= 0) {
        outPin = value;
        return true;
    }

    LOGD("GET_CUR for clock selector %u failed: errno=%d %s", selectorId, errno, strerror(errno));
    return false;
}

bool USBAudioInterface::setClockSelectorValue(uint8_t selectorId, uint8_t pinValue) const {
    if (m_deviceFd < 0 || m_controlInterfaceNumber < 0) {
        return false;
    }

    uint8_t value = pinValue;
    struct usbdevfs_ctrltransfer ctrl = {};
    ctrl.bRequestType = 0x21; // Class, Interface, Host to Device
    ctrl.bRequest = UAC_SET_CUR;
    ctrl.wValue = static_cast<uint16_t>(UAC2_CX_CLOCK_SELECTOR) << 8;
    ctrl.wIndex = (static_cast<uint16_t>(selectorId) << 8) |
                  static_cast<uint16_t>(m_controlInterfaceNumber & 0xFF);
    ctrl.wLength = 1;
    ctrl.timeout = 1000;
    ctrl.data = &value;

    int result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
    if (result >= 0) {
        return true;
    }

    LOGI("SET_CUR for clock selector %u pin %u failed: errno=%d %s", selectorId, pinValue, errno, strerror(errno));
    return false;
}

bool USBAudioInterface::evaluateClockValidity(uint8_t clockId, const ClockSourceDetails* details, int maxRetries) {
    if (!details) {
        return true;
    }

    if (!isControlReadable(details->bmControls, UAC2_CS_CONTROL_CLOCK_VALID)) {
        return true;
    }

    if (m_deviceFd < 0) {
        return false;
    }

    const int retries = std::max(1, maxRetries);
    std::vector<int> interfaceCandidates;
    interfaceCandidates.reserve(3);
    if (m_controlInterfaceNumber >= 0) {
        interfaceCandidates.push_back(m_controlInterfaceNumber);
    }
    if (m_streamInterfaceNumber >= 0) {
        interfaceCandidates.push_back(m_streamInterfaceNumber);
    }
    interfaceCandidates.push_back(-1); // entity-only fallback

    uint8_t valid = 0;
    bool anySuccess = false;

    for (int attempt = 0; attempt < retries; ++attempt) {
        for (int iface : interfaceCandidates) {
            struct usbdevfs_ctrltransfer ctrl = {};
            ctrl.bRequestType = 0xA1; // Class, Interface, Device to Host
            ctrl.bRequest = UAC_GET_CUR;
            ctrl.wValue = (UAC2_CS_CONTROL_CLOCK_VALID << 8);
            ctrl.wIndex = (static_cast<uint16_t>(clockId) << 8) |
                          static_cast<uint16_t>((iface >= 0) ? (iface & 0xFF) : 0x00);
            ctrl.wLength = 1;
            ctrl.timeout = 1000;
            ctrl.data = &valid;

            int result = ioctl(m_deviceFd, USBDEVFS_CONTROL, &ctrl);
            if (result >= 0) {
                anySuccess = true;
                if (valid) {
                    return true;
                }
            } else if (errno != EBUSY) {
                LOGD("Clock validity GET_CUR failed (clock=%u iface=%d errno=%d %s)",
                     clockId, iface, errno, strerror(errno));
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!anySuccess) {
        LOGD("Clock validity check returned no successful responses for clock %u", clockId);
    }
    return false;
}

int USBAudioInterface::resolveClockEntity(int entityId, bool validate, std::unordered_set<int>& visited) {
    if (entityId <= 0) {
        return -1;
    }

    if (!visited.insert(entityId).second) {
        LOGI("Detected recursive clock topology involving entity %d", entityId);
        return -1;
    }

    int resolvedId = -1;

    if (const ClockSourceDetails* source = findClockSourceDetails(static_cast<uint8_t>(entityId))) {
        if (!validate || evaluateClockValidity(static_cast<uint8_t>(entityId), source, 20)) {
            resolvedId = entityId;
        }
    } else if (const ClockSelectorDetails* selector = findClockSelectorDetails(static_cast<uint8_t>(entityId))) {
        if (!selector->inputs.empty()) {
            uint8_t currentPin = 0;
            bool haveCurrentPin = false;

            if (isControlReadable(selector->bmControls, UAC2_CX_CLOCK_SELECTOR)) {
                uint8_t pinValue = 0;
                if (getClockSelectorValue(selector->id, pinValue) &&
                    pinValue >= 1 && pinValue <= selector->inputs.size()) {
                    currentPin = pinValue;
                    haveCurrentPin = true;
                }
            }

            struct Candidate {
                uint8_t pinValue;
                uint8_t sourceId;
                bool isCurrent;
            };

            std::vector<Candidate> candidates;
            candidates.reserve(selector->inputs.size());

            for (size_t idx = 0; idx < selector->inputs.size(); ++idx) {
                uint8_t pinValue = static_cast<uint8_t>(idx + 1);
                Candidate candidate{pinValue, selector->inputs[idx], haveCurrentPin && pinValue == currentPin};
                if (candidate.isCurrent) {
                    candidates.insert(candidates.begin(), candidate);
                } else {
                    candidates.push_back(candidate);
                }
            }

            const bool writable = isControlWritable(selector->bmControls, UAC2_CX_CLOCK_SELECTOR);

            for (const auto& candidate : candidates) {
                if (candidate.sourceId == 0) {
                    continue;
                }

                const bool requiresSwitch = !candidate.isCurrent;
                if (requiresSwitch && !writable) {
                    continue;
                }

                uint8_t restorePin = currentPin;
                bool switchApplied = !requiresSwitch;
                if (requiresSwitch) {
                    if (setClockSelectorValue(selector->id, candidate.pinValue)) {
                        switchApplied = true;
                        currentPin = candidate.pinValue;
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    } else {
                        LOGI("Unable to switch clock selector %u to pin %u", selector->id, candidate.pinValue);
                    }
                }

                if (switchApplied) {
                    int childResolved = resolveClockEntity(candidate.sourceId, validate, visited);
                    if (childResolved >= 0) {
                        resolvedId = childResolved;
                        break;
                    }
                }

                if (requiresSwitch && switchApplied) {
                    if (restorePin >= 1 && writable) {
                        setClockSelectorValue(selector->id, restorePin);
                        currentPin = restorePin;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            }
        }
    } else if (const ClockMultiplierDetails* multiplier = findClockMultiplierDetails(static_cast<uint8_t>(entityId))) {
        resolvedId = resolveClockEntity(multiplier->sourceId, validate, visited);
    }

    visited.erase(entityId);
    return resolvedId;
}

bool USBAudioInterface::resolveAndApplyClockSelection(bool validate) {
    int startEntity = -1;
    if (m_streamClockEntityId >= 0) {
        startEntity = m_streamClockEntityId;
    } else if (m_clockSourceId >= 0) {
        startEntity = m_clockSourceId;
    } else if (!m_clockSourceMap.empty()) {
        startEntity = m_clockSourceMap.begin()->first;
    }

    if (startEntity < 0) {
        return false;
    }

    std::unordered_set<int> visited;
    int resolved = resolveClockEntity(startEntity, validate, visited);
    if (resolved >= 0) {
        m_clockSourceId = resolved;
        return true;
    }
    return false;
}

bool USBAudioInterface::checkClockValidity(int clockId, int maxRetries) {
    const ClockSourceDetails* details = findClockSourceDetails(static_cast<uint8_t>(clockId));
    return evaluateClockValidity(static_cast<uint8_t>(clockId), details, maxRetries);
}

void USBAudioInterface::releaseUrbResources() {
    if (m_urbs) {
        if (m_deviceFd >= 0) {
            // Request cancellation for every URB that might still be owned by the kernel.
            for (int i = 0; i < NUM_URBS; ++i) {
                if (!m_urbs[i]) {
                    continue;
                }

                int discardResult = ioctl(m_deviceFd, USBDEVFS_DISCARDURB, m_urbs[i]);
                if (discardResult != 0 && errno != EINVAL) {
                    LOGE("Failed to discard URB[%d]: %s (errno %d)", i, strerror(errno), errno);
                }
            }

            // Reap any URBs that were still in flight to ensure the kernel has fully detached from them.
            struct usbdevfs_urb* completed = nullptr;
            while (ioctl(m_deviceFd, USBDEVFS_REAPURBNDELAY, &completed) == 0) {
                // Drain all completions; no additional work is required here because
                // the URB memory is still owned by this object.
            }
        }

        for (int i = 0; i < NUM_URBS; ++i) {
            if (m_urbs[i]) {
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

    // Clear endpoint halt condition before starting streaming
    // This is critical - Linux USB audio driver does this to clear stale errors
    LOGI("Clearing endpoint halt on 0x%02x before URB submission", m_audioInEndpoint);
    int clearResult = ioctl(m_deviceFd, USBDEVFS_CLEAR_HALT, &m_audioInEndpoint);
    if (clearResult < 0) {
        LOGI("Clear halt failed (errno %d: %s) - may not be needed", errno, strerror(errno));
    } else {
        LOGI("Endpoint halt cleared successfully");
    }

    return true;
}

void USBAudioInterface::resetStreamingState() {
    m_wasStreaming = false;
    m_lastReapedUrbAddress = nullptr;
    m_consecutiveSameUrbCount = 0;
    m_recentReapCheckpoint = 0;
    m_stuckUrbDetected = false;
    m_callCount = 0;
    m_attemptCount = 0;
    m_submitErrorCount = 0;
    m_reapCount = 0;
    m_reapErrorCount = 0;
    m_eagainCount = 0;
    m_reapAttemptCount = 0;
    m_notStreamingCount = 0;
    m_noFramesCount = 0;
    m_pendingData.clear();
    m_pendingReadOffset = 0;
    m_totalSubmitted = 0;
    m_nextSubmitIndex = 0;
    m_currentFrameNumber = 0;
    m_frameNumberInitialized = false;
}

bool USBAudioInterface::flushIsochronousEndpoint() {
    if (m_deviceFd < 0) {
        LOGE("Cannot flush isochronous endpoint: invalid device fd");
        return false;
    }

    if (!m_endpointInfoReady) {
        LOGE("Cannot flush isochronous endpoint: endpoint information not ready");
        return false;
    }

    resetStreamingState();
    bool success = true;

    // Force the interface into the idle alternate setting before clearing any state.
    if (m_streamInterfaceNumber >= 0) {
        if (!setInterface(m_streamInterfaceNumber, 0)) {
            LOGE("Failed to set interface %d to alt 0 during flush", m_streamInterfaceNumber);
            success = false;
        }
    }

    // Best-effort cancel of any URBs that might still be owned by the kernel for this fd.
    if (m_urbs && m_urbsInitialized) {
        int cancelled = 0;
        for (int i = 0; i < NUM_URBS; ++i) {
            if (!m_urbs[i]) {
                continue;
            }
            if (ioctl(m_deviceFd, USBDEVFS_DISCARDURB, m_urbs[i]) == 0) {
                ++cancelled;
            } else if (errno != EINVAL && errno != ENODEV) {
                LOGE("DISCARDURB failed for URB[%d]: %s (errno %d)", i, strerror(errno), errno);
                success = false;
            }
        }

        for (int i = 0; i < cancelled; ++i) {
            struct usbdevfs_urb* reaped = nullptr;
            if (ioctl(m_deviceFd, USBDEVFS_REAPURB, &reaped) < 0) {
                if (errno != EINVAL && errno != ENODEV) {
                    LOGE("REAPURB during flush failed: %s (errno %d)", strerror(errno), errno);
                    success = false;
                }
                break;
            }
        }
    }

    // Clear any lingering data toggle on the streaming endpoint.
    if (m_audioInEndpoint != 0 && m_streamInterfaceNumber >= 0) {
        unsigned int endpoint = static_cast<unsigned int>(m_audioInEndpoint);
        struct usbdevfs_ioctl clear = {};
        clear.ifno = m_streamInterfaceNumber;
        clear.ioctl_code = USBDEVFS_CLEAR_HALT;
        clear.data = &endpoint;

        int clearResult = ioctl(m_deviceFd, USBDEVFS_IOCTL, &clear);
        if (clearResult == 0) {
            LOGI("Cleared halt on endpoint 0x%02x", endpoint);
        } else if (errno == EINVAL || errno == ENOTTY) {
            LOGI("CLEAR_HALT not supported for endpoint 0x%02x (errno %d)", endpoint, errno);
        } else {
            LOGE("Failed to clear halt on endpoint 0x%02x: %s (errno %d)", endpoint, strerror(errno), errno);
            success = false;
        }
    }

    // Synchronize with the device's current USB frame number.
    unsigned int currentFrame = 0;
    if (ioctl(m_deviceFd, USBDEVFS_GET_CURRENT_FRAME, &currentFrame) == 0) {
        m_currentFrameNumber = static_cast<int>(currentFrame);
        m_frameNumberInitialized = true;
        LOGI("Flushed endpoint and synchronized to USB frame %u", currentFrame);
    } else if (errno != ENOTTY) {
        LOGE("Failed to read current USB frame during flush: %s (errno %d)", strerror(errno), errno);
        success = false;
    }

    // Allow the device a brief window to settle before we resume streaming.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    return success;
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
    if (m_isStreaming) {
        LOGI("Stopping USB audio streaming");
        m_isStreaming = false;

        // Cancel any pending URBs before disabling the interface
        if (m_urbs && m_deviceFd >= 0) {
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
    } else {
        LOGI("stopStreaming called while already stopped");
    }

    releaseUrbResources();
    resetStreamingState();

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
    if (!setInterfaceWithRetry(streamingInterface, 0, 5)) {
        LOGE("Failed to reset streaming interface to alt 0");
        return false;
    }
    usleep(50000); // 50ms delay
    
    // STEP 2: Configure sample rate BEFORE enabling the interface
    // Linux USB audio does: usb_set_interface(alt 0) -> init_sample_rate() -> usb_set_interface(alt 1)
    LOGI("Step 2: Configuring sample rate to %d Hz on endpoint 0x%02x", m_sampleRate, streamingEndpoint);
    if (!configureSampleRate(m_sampleRate)) {
        LOGI("Sample rate configuration reported no explicit success; proceeding with device defaults");
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
    if (!setInterfaceWithRetry(streamingInterface, streamingAltSetting, 5)) {
        LOGE("Failed to enable streaming interface alt %d", streamingAltSetting);
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

    
    if (!m_wasStreaming) {
        LOGI("Streaming started - resetting URB queue state");
        releaseUrbResources();
        resetStreamingState();
        if (!ensureUrbResources()) {
            return 0;
        }
        m_wasStreaming = true;
    }

    m_callCount++;

    // Log first few calls with state information
    if (m_callCount <= 5 || m_callCount % 1000 == 0) {
        LOGI("readAudioData called (count=%d): bufferSize=%zu, isStreaming=%d, m_wasStreaming=%d, urbsInit=%d, totalSub=%d, fd=%d", 
             m_callCount, bufferSize, m_isStreaming, m_wasStreaming, m_urbsInitialized, m_totalSubmitted, m_deviceFd);
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
                LOGI("Submitted initial URB[%d] (%d/%d, %zu packets)",
                     m_nextSubmitIndex, m_totalSubmitted, NUM_URBS, m_packetsPerUrb);
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

    // Only hand off complete 84-channel frames; leaving partial frames staged avoids audible artifacts.
    auto drainPendingData = [&](uint8_t* dest, size_t capacity) -> size_t {
        if (frameSize == 0 || capacity < frameSize || m_pendingData.empty()) {
            return 0;
        }

        const size_t available = m_pendingData.size() - m_pendingReadOffset;
        if (available < frameSize) {
            return 0;
        }

        const size_t maxFramesByCapacity = capacity / frameSize;
        if (maxFramesByCapacity == 0) {
            return 0;
        }

        const size_t availableFrames = available / frameSize;
        const size_t framesToCopy = std::min(maxFramesByCapacity, availableFrames);
        if (framesToCopy == 0) {
            return 0;
        }

        const size_t bytesToCopy = framesToCopy * frameSize;
        memcpy(dest, m_pendingData.data() + m_pendingReadOffset, bytesToCopy);
        m_pendingReadOffset += bytesToCopy;
        if (m_pendingReadOffset >= m_pendingData.size()) {
            m_pendingData.clear();
            m_pendingReadOffset = 0;
        }
        return bytesToCopy;
    };

    auto appendPendingData = [&](const uint8_t* src, size_t length) {
        if (length == 0) {
            return;
        }

        if (!m_pendingData.empty() && m_pendingReadOffset > 0) {
            if (m_pendingReadOffset >= m_pendingData.size()) {
                m_pendingData.clear();
            } else {
                const size_t unread = m_pendingData.size() - m_pendingReadOffset;
                memmove(m_pendingData.data(),
                        m_pendingData.data() + m_pendingReadOffset,
                        unread);
                m_pendingData.resize(unread);
            }
            m_pendingReadOffset = 0;
        }

        const size_t currentSize = m_pendingData.size();
        m_pendingData.resize(currentSize + length);
        memcpy(m_pendingData.data() + currentSize, src, length);

        if (m_pendingData.size() > MAX_PENDING_BUFFER_BYTES) {
            LOGE("Pending staging buffer exceeded %zu bytes (current=%zu). Downstream consumer is not keeping up.",
                 static_cast<size_t>(MAX_PENDING_BUFFER_BYTES), m_pendingData.size());
        }
    };

    size_t total_bytes_accumulated = drainPendingData(buffer, bufferSize);
    if (total_bytes_accumulated >= bufferSize) {
        return total_bytes_accumulated;
    }

    int urbs_reaped_this_call = 0;
    const int MAX_REAPS_PER_CALL = 32; // Safety limit
    bool resetTriggered = false;

    auto handleCompletedUrb = [&](struct usbdevfs_urb* completed_urb, int loopIndex) {
        int urb_index = static_cast<int>(reinterpret_cast<intptr_t>(completed_urb->usercontext));
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
                resetStreamingState();
                resetTriggered = true;

                LOGI("All URBs cancelled - will reinitialize on next readAudioData() call");
                return;
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
                if (m_reapCount <= 50 || (m_reapCount % 1000 == 0 && error_count <= 2)) {
                    LOGE("URB[%d] packet[%zu] error: status=%d, actual=%u",
                         urb_index, pkt, completed_urb->iso_frame_desc[pkt].status,
                         completed_urb->iso_frame_desc[pkt].actual_length);
                }
            }
        }

        if (total_actual > 0 && total_bytes_accumulated < bufferSize) {
            uint8_t* urbData = static_cast<uint8_t*>(completed_urb->buffer);
            LOG_FATAL_IF(urbData == nullptr, "URB[%d] buffer is null", urb_index);
            LOG_FATAL_IF(m_urbBufferSize == 0, "URB[%d] buffer size is zero", urb_index);

            size_t packetOffset = 0;
            for (size_t pkt = 0; pkt < m_packetsPerUrb; ++pkt) {
                unsigned int packetLength = completed_urb->iso_frame_desc[pkt].actual_length;
                if (packetLength > 0) {
                    LOG_FATAL_IF(packetOffset >= m_urbBufferSize,
                                 "URB[%d] packetOffset=%zu exceeds buffer=%zu (pkt=%zu)",
                                 urb_index, packetOffset, m_urbBufferSize, pkt);

                    if (packetOffset + packetLength > m_urbBufferSize) {
                        LOGE("Packet length %u exceeds URB buffer bounds (offset=%zu, size=%zu)",
                             packetLength, packetOffset, m_urbBufferSize);
                        packetLength = std::min<unsigned int>(packetLength,
                                                              static_cast<unsigned int>(m_urbBufferSize - packetOffset));
                    }

                    size_t bytesToCopy = 0;
                    if (total_bytes_accumulated < bufferSize) {
                        const size_t remainingDest = bufferSize - total_bytes_accumulated;
                        const size_t remainingFrames = frameSize > 0 ? (remainingDest / frameSize) : 0;
                        const size_t copyCapacity = remainingFrames * frameSize;
                        bytesToCopy = std::min(static_cast<size_t>(packetLength), copyCapacity);
                        LOG_FATAL_IF(packetOffset + bytesToCopy > m_urbBufferSize,
                                     "URB[%d] copy range exceeds source bounds (offset=%zu, copy=%zu, size=%zu)",
                                     urb_index, packetOffset, bytesToCopy, m_urbBufferSize);

                        if (bytesToCopy > 0) {
                            memcpy(buffer + total_bytes_accumulated, urbData + packetOffset, bytesToCopy);
                            total_bytes_accumulated += bytesToCopy;
                        }
                    }

                    const size_t spillover = static_cast<size_t>(packetLength) - bytesToCopy;
                    if (spillover > 0) {
                        LOG_FATAL_IF(packetOffset + bytesToCopy + spillover > m_urbBufferSize,
                                     "URB[%d] spillover range exceeds source bounds (offset=%zu, copy=%zu, spill=%zu, size=%zu)",
                                     urb_index, packetOffset, bytesToCopy, spillover, m_urbBufferSize);
                        appendPendingData(urbData + packetOffset + bytesToCopy, spillover);
                    }
                }
                packetOffset += m_isoPacketSize;
            }

            if (m_reapCount <= 20 || m_reapCount % 100 == 0) {
                size_t frameBytes = static_cast<size_t>(m_channelCount) * static_cast<size_t>(m_bytesPerSample);
                size_t samplesPerChannel = frameBytes > 0 ? total_actual / frameBytes : 0;
                LOGI("ISO URB[%d] reaped (reap#%d, loop#%d): %zu bytes (%zu samples/ch), accumulated=%zu",
                     urb_index, m_reapCount, loopIndex, total_actual, samplesPerChannel, total_bytes_accumulated);
            }
        }

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
    };

    auto reapCompletions = [&](bool blocking) {
        bool reapedAny = false;
        int loops = blocking ? 1 : MAX_REAPS_PER_CALL;
        for (int reap_loop = 0; reap_loop < loops; ++reap_loop) {
            struct usbdevfs_urb* completed_urb = nullptr;
            int command = blocking ? USBDEVFS_REAPURB : USBDEVFS_REAPURBNDELAY;
            int reap_result = ioctl(m_deviceFd, command, &completed_urb);
            int saved_errno = errno;

            m_reapAttemptCount++;

            if (reap_result < 0) {
                if (!blocking && saved_errno == EAGAIN) {
                    if (reap_loop == 0) {
                        if (++m_eagainCount <= 20 || m_eagainCount % 1000 == 0) {
                            LOGD("No URB ready (EAGAIN, count=%d), totalSub=%d", m_eagainCount, m_totalSubmitted);
                        }
                    }
                    break;
                }

                if (saved_errno == EINTR) {
                    continue;
                }

                if (++m_reapErrorCount <= 20) {
                    LOGE("URB reap error (cmd=%s, result=%d, errno=%d: %s)",
                         blocking ? "REAPURB" : "REAPURBNDELAY",
                         reap_result, saved_errno, strerror(saved_errno));
                }
                break;
            }

            if (!completed_urb) {
                break;
            }

            reapedAny = true;
            handleCompletedUrb(completed_urb, blocking ? -1 : reap_loop);

            if (resetTriggered) {
                break;
            }

            if (!blocking && total_bytes_accumulated >= bufferSize) {
                break;
            }
        }
        return reapedAny;
    };

    reapCompletions(false);

    if (resetTriggered) {
        return total_bytes_accumulated;
    }

    if (total_bytes_accumulated == 0 && m_isStreaming) {
        auto waitStart = std::chrono::steady_clock::now();
        bool reapedAfterWait = reapCompletions(true);
        if (reapedAfterWait) {
            auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - waitStart);
            if (waited.count() > 0 && (m_reapCount <= 20 || m_reapCount % 1000 == 0)) {
                LOGD("Blocking wait for URB completed in %lld us", static_cast<long long>(waited.count()));
            }
            reapCompletions(false);
        }
    }

    if (resetTriggered) {
        return total_bytes_accumulated;
    }

    if (urbs_reaped_this_call > 1 && (m_reapCount <= 50 || m_reapCount % 100 == 0)) {
        LOGI("Reaped %d URBs in single call (reap#%d), total bytes=%zu",
             urbs_reaped_this_call, m_reapCount, total_bytes_accumulated);
    }

    if (total_bytes_accumulated < bufferSize) {
        total_bytes_accumulated += drainPendingData(buffer + total_bytes_accumulated,
                                                   bufferSize - total_bytes_accumulated);
    }

    if (frameSize > 0) {
        const size_t remainder = total_bytes_accumulated % frameSize;
        if (remainder != 0) {
            appendPendingData(buffer + (total_bytes_accumulated - remainder), remainder);
            total_bytes_accumulated -= remainder;
        }
    }

    return total_bytes_accumulated;
}

bool USBAudioInterface::setTargetSampleRate(int sampleRate) {
    if (sampleRate <= 0) {
        LOGE("Invalid sample rate requested: %d", sampleRate);
        return false;
    }

    if (m_deviceFd < 0) {
        LOGE("Cannot set sample rate; device handle is invalid");
        return false;
    }

    if (m_isStreaming) {
        LOGE("Cannot change sample rate while streaming is active");
        return false;
    }

    // Query actual device state instead of comparing against stored m_sampleRate
    // This ensures we always know what the device is REALLY running at
    uint32_t currentDeviceRate = 0;
    const char* querySource = nullptr;
    if (queryCurrentSampleRate(currentDeviceRate, &querySource)) {
        if (currentDeviceRate == static_cast<uint32_t>(sampleRate)) {
            LOGI("Device already running at %d Hz (verified via %s)", sampleRate, querySource ? querySource : "unknown");
            m_sampleRate = sampleRate;  // Sync our stored value to match reality
            m_effectiveSampleRate = static_cast<double>(sampleRate);
            return true;
        }
        LOGI("Device currently at %u Hz, changing to %d Hz", currentDeviceRate, sampleRate);
    } else {
        LOGI("Could not query current device rate; attempting to set %d Hz", sampleRate);
    }

    int previousRate = m_sampleRate;
    double previousEffective = m_effectiveSampleRate;

    m_sampleRate = sampleRate;

    if (!configureSampleRate(sampleRate)) {
        LOGE("Device rejected sample rate %d Hz; restoring previous rate %d Hz", sampleRate, previousRate);
        m_sampleRate = previousRate;
        m_effectiveSampleRate = previousEffective;
        return false;
    }

    LOGI("Sample rate updated to %d Hz", sampleRate);
    return true;
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

int USBAudioInterface::getEffectiveSampleRateRounded() const {
    if (m_effectiveSampleRate <= 0.0) {
        return m_sampleRate;
    }
    return static_cast<int>(std::lround(m_effectiveSampleRate));
}

void USBAudioInterface::updateEffectiveSampleRate() {
    double frameBytes = static_cast<double>(m_channelCount) * static_cast<double>(m_bytesPerSample);
    m_effectiveSampleRate = static_cast<double>(m_sampleRate);

    if (!m_endpointInfoReady || m_isoPacketSize == 0 || frameBytes <= 0.0) {
        return;
    }

    size_t intervalFactor = std::max<size_t>(1, m_packetsPerServiceInterval);
    double baseRate = (m_isHighSpeed || m_isSuperSpeed) ? 8000.0 : 1000.0;
    double intervalsPerSecond = baseRate / static_cast<double>(intervalFactor);
    double framesPerInterval = static_cast<double>(m_isoPacketSize) / frameBytes;
    double computed = framesPerInterval * intervalsPerSecond;

    if (computed > 0.0) {
        m_effectiveSampleRate = computed;
        LOGI("Derived effective sample rate: %.2f Hz (frameBytes=%.0f, baseRate=%.0f, intervalFactor=%zu)",
             m_effectiveSampleRate, frameBytes, baseRate, intervalFactor);
    }
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

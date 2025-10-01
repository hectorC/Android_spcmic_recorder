#pragma once

#include <cstdint>

// USB Audio Class Protocol Definitions
// Based on USB Audio Class 2.0 specification

// USB Audio Class Codes
#define USB_CLASS_AUDIO                 0x01
#define USB_SUBCLASS_AUDIOCONTROL      0x01
#define USB_SUBCLASS_AUDIOSTREAMING    0x02

// USB Audio Class Request Codes
#define UAC_SET_CUR                    0x01
#define UAC_GET_CUR                    0x81
#define UAC_SET_MIN                    0x02
#define UAC_GET_MIN                    0x82
#define UAC_SET_MAX                    0x03
#define UAC_GET_MAX                    0x83
#define UAC_SET_RES                    0x04
#define UAC_GET_RES                    0x84

// Audio Control Selectors
#define UAC_SAMPLING_FREQ_CONTROL      0x01
#define UAC_PITCH_CONTROL              0x02
#define UAC_MUTE_CONTROL               0x01
#define UAC_VOLUME_CONTROL             0x02

// Audio Format Type Codes
#define UAC_FORMAT_TYPE_I              0x01
#define UAC_FORMAT_TYPE_II             0x02
#define UAC_FORMAT_TYPE_III            0x03

// Audio Data Format Codes
#define UAC_FORMAT_PCM                 0x0001
#define UAC_FORMAT_PCM8                0x0002
#define UAC_FORMAT_IEEE_FLOAT          0x0003

// Endpoint Attributes
#define UAC_EP_ATTR_ADAPTIVE           0x01
#define UAC_EP_ATTR_ASYNC              0x02
#define UAC_EP_ATTR_SYNC               0x03

// USB Audio Class Descriptors
struct UACControlHeader {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint16_t bcdADC;
    uint16_t wTotalLength;
    uint8_t bInCollection;
    uint8_t baInterfaceNr[];
} __attribute__((packed));

struct UACInputTerminal {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bTerminalID;
    uint16_t wTerminalType;
    uint8_t bAssocTerminal;
    uint8_t bNrChannels;
    uint16_t wChannelConfig;
    uint8_t iChannelNames;
    uint8_t iTerminal;
} __attribute__((packed));

struct UACOutputTerminal {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bTerminalID;
    uint16_t wTerminalType;
    uint8_t bAssocTerminal;
    uint8_t bSourceID;
    uint8_t iTerminal;
} __attribute__((packed));

struct UACFormatTypeI {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bFormatType;
    uint8_t bNrChannels;
    uint8_t bSubframeSize;
    uint8_t bBitResolution;
    uint8_t bSamFreqType;
    uint8_t tSamFreq[];
} __attribute__((packed));

struct UACEndpointDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    uint8_t bRefresh;
    uint8_t bSynchAddress;
} __attribute__((packed));

struct UACAudioEndpoint {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bmAttributes;
    uint8_t bLockDelayUnits;
    uint16_t wLockDelay;
} __attribute__((packed));

// Sample Rate Structure for 24-bit samples
struct SampleRate24 {
    uint8_t byte0;
    uint8_t byte1;
    uint8_t byte2;
} __attribute__((packed));

// Channel Configuration for 84 channels
// This would be device-specific based on the SPCMic configuration
struct ChannelConfig84 {
    static const int CHANNEL_COUNT = 84;
    static const int BYTES_PER_SAMPLE = 3;  // 24-bit
    static const int FRAME_SIZE = CHANNEL_COUNT * BYTES_PER_SAMPLE;  // 252 bytes per frame
};
#include "uac_protocol.h"
#include <android/log.h>

#define LOG_TAG "UACProtocol"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// USB Audio Class protocol implementation
// This file contains utility functions for working with UAC descriptors
// and protocol-specific operations

namespace UACProtocol {
    
    bool parseSampleRate(const uint8_t* data, int& sampleRate) {
        if (!data) return false;
        
        // Parse 24-bit sample rate (little endian)
        sampleRate = data[0] | (data[1] << 8) | (data[2] << 16);
        return true;
    }
    
    void encodeSampleRate(int sampleRate, uint8_t* data) {
        if (!data) return;
        
        // Encode 24-bit sample rate (little endian)
        data[0] = sampleRate & 0xFF;
        data[1] = (sampleRate >> 8) & 0xFF;
        data[2] = (sampleRate >> 16) & 0xFF;
    }
    
    int calculateMaxPacketSize(int sampleRate, int channels, int bytesPerSample) {
        // Calculate maximum packet size for isochronous transfers
        // For USB audio, this is typically based on 1ms frames
        
        int samplesPerMs = sampleRate / 1000;
        int maxPacketSize = samplesPerMs * channels * bytesPerSample;
        
        // Add some margin for timing variations
        maxPacketSize += (maxPacketSize / 10);  // 10% margin
        
        LOGI("Calculated max packet size: %d bytes for %dHz %dch %dbytes", 
             maxPacketSize, sampleRate, channels, bytesPerSample);
        
        return maxPacketSize;
    }
    
    bool validateAudioFormat(int sampleRate, int channels, int bytesPerSample) {
        // Validate that the audio format is supported
        
        // Check sample rate (typical professional audio rates)
        if (sampleRate != 44100 && sampleRate != 48000 && 
            sampleRate != 88200 && sampleRate != 96000 &&
            sampleRate != 176400 && sampleRate != 192000) {
            LOGE("Unsupported sample rate: %d", sampleRate);
            return false;
        }
        
        // Check channel count (1-84 for SPCMic)
        if (channels < 1 || channels > 84) {
            LOGE("Unsupported channel count: %d", channels);
            return false;
        }
        
        // Check bytes per sample (16-bit, 24-bit, 32-bit)
        if (bytesPerSample != 2 && bytesPerSample != 3 && bytesPerSample != 4) {
            LOGE("Unsupported bytes per sample: %d", bytesPerSample);
            return false;
        }
        
        LOGI("Audio format validated: %dHz, %dch, %d bytes/sample", 
             sampleRate, channels, bytesPerSample);
        
        return true;
    }
    
    void logAudioDescriptor(const UACFormatTypeI* desc) {
        if (!desc) return;
        
        LOGI("UAC Format Type I Descriptor:");
        LOGI("  Length: %d", desc->bLength);
        LOGI("  Type: 0x%02x", desc->bDescriptorType);
        LOGI("  Subtype: 0x%02x", desc->bDescriptorSubtype);
        LOGI("  Format Type: %d", desc->bFormatType);
        LOGI("  Channels: %d", desc->bNrChannels);
        LOGI("  Subframe Size: %d", desc->bSubframeSize);
        LOGI("  Bit Resolution: %d", desc->bBitResolution);
        LOGI("  Sample Freq Type: %d", desc->bSamFreqType);
    }
    
    void logEndpointDescriptor(const UACEndpointDescriptor* desc) {
        if (!desc) return;
        
        LOGI("UAC Endpoint Descriptor:");
        LOGI("  Length: %d", desc->bLength);
        LOGI("  Type: 0x%02x", desc->bDescriptorType);
        LOGI("  Address: 0x%02x", desc->bEndpointAddress);
        LOGI("  Attributes: 0x%02x", desc->bmAttributes);
        LOGI("  Max Packet Size: %d", desc->wMaxPacketSize);
        LOGI("  Interval: %d", desc->bInterval);
    }
}
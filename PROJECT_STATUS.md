# Android SPCMic Recorder - Project Status Report

**Date:** October 1, 2025  
**Device:** SPCMic USB Audio Interface (VID: 22564, PID: 10208)  
**Target:** 84-channel audio recording at 48kHz, 24-bit via USB Audio Class 2.0  
**Platform:** Android (Samsung SM-S938W), compileSdk 35, NDK with native USB access

---

## Project Overview

Developing an Android app to record 84 channels of audio from a SPCMic USB device at 48kHz/24-bit using direct USB isochronous transfers via native code (C++ with usbdevfs ioctls). The device is USB Audio Class 2.0 compliant and works perfectly on Windows and Linux with standard drivers.

---

## Current Architecture

### Implementation Approach
- **Native C++ via JNI** for USB access
- **Direct usbdevfs URB (USB Request Block) ioctls** for isochronous transfers
- **No Android USB Host API** (Java layer inadequate for 84 channels)
- **UAC 2.0 initialization sequence**: alt 0 → SET_CUR sample rate → alt 1

### Key Components
1. `usb_audio_interface.cpp` (777 lines) - Main URB management
2. `multichannel_recorder.cpp` (293 lines) - Recording thread
3. `uac_protocol.cpp` - USB Audio Class 2.0 protocol handling

### Device Configuration
- **Interface 3, Alternate Setting 1**
- **Endpoint 0x81** (IN, isochronous)
- **maxPacketSize: 5104 bytes**
- **Interval: 1ms** (USB full-speed frame)
- **Expected data rate**: 84 ch × 48000 Hz × 3 bytes = 12,096,000 bytes/sec (~12 MB/s)
- **Expected for 5 seconds**: ~60 MB of audio data

---

## Successfully Implemented Features

### ✅ USB Device Initialization
- Device detection and permission handling
- USB file descriptor acquisition (FD=112)
- Interface claiming and alternate setting configuration
- Sample rate configuration via SET_CUR control transfer (48kHz confirmed)

### ✅ URB Infrastructure
- Dynamic URB allocation with iso_frame_desc arrays
- Multiple packets per URB for batch processing
- URB submission via `USBDEVFS_SUBMITURB` ioctl
- URB reaping via `USBDEVFS_REAPURB` ioctl
- Proper URB lifecycle management (submit → reap → resubmit)

### ✅ Multi-URB Reaping Loop
- Successfully reaping 8-32 URBs per call
- Reaping rate: ~9,600 URBs/second (80x improvement over initial 146/sec)
- Data accumulation from multiple reaped URBs
- Immediate URB resubmission to maintain queue depth

### ✅ Error Detection and Logging
- Packet-level error checking (iso_frame_desc[].status)
- Comprehensive logging of URB states and errors
- Detection of EOVERFLOW, EMSGSIZE, EPROTO errors

---

## Critical Blocking Issue

### **Android Kernel URB Size Limitation vs Device Requirements**

The project has hit a **fundamental incompatibility** between Android's kernel limitations and the device's hardware requirements:

#### Device Requirements (Fixed, Hardware-Level)
- **5104 bytes per isochronous packet** (endpoint maxPacketSize)
- This is non-negotiable for 84 channels @ 48kHz/24-bit
- Device has only **2 alternate settings**:
  - Alt 0: No endpoints (disabled)
  - Alt 1: 1 endpoint with 5104-byte maxPacketSize
- No other packet size options available

#### Android Kernel Limitations (Discovered Through Testing)
- **Maximum URB buffer size**: ~23,500 bytes total
- With 8 packets per URB: **2944 bytes per packet maximum**
- Exceeding this limit causes **EMSGSIZE (errno 90)** - "Message too long"

#### The Incompatibility
```
Device sends:     5104 bytes per packet
Kernel accepts:   2944 bytes per packet maximum
Shortfall:        2160 bytes per packet (42% data loss per packet)
Result:           EOVERFLOW (status -75) on every packet
```

When the device sends 5104 bytes to our 2944-byte buffer:
1. Kernel gets EOVERFLOW error
2. **Entire packet is discarded** (no partial data captured)
3. URB is resubmitted with same too-small buffer
4. Infinite loop of overflow errors

---

## Detailed Testing History

### Configuration Matrix Tested
| URBs | Packets/URB | Bytes/Packet | Total Buffer | Result |
|------|-------------|--------------|--------------|---------|
| 4 | 8 | 2816 | 22,528 | ✅ Submits, EOVERFLOW, 9KB captured |
| 16 | 8 | 2816 | 22,528 | ✅ Submits, EOVERFLOW, 9KB captured |
| 32 | 1 | 5104 | 5,104 | ❌ EMSGSIZE (errno 90) |
| 32 | 1 | 4096 | 4,096 | ❌ EMSGSIZE (errno 90) |
| 32 | 1 | 2816 | 2,816 | ✅ Submits, EOVERFLOW |
| 16 | 8 | 5104 | 40,832 | ❌ EMSGSIZE (errno 90) |
| 32 | 8 | 1024 | 8,192 | ✅ Submits, EPROTO (status -71) |
| 32 | 8 | 3072 | 24,576 | ❌ EMSGSIZE (errno 90) |
| 32 | 8 | 2944 | 23,552 | ✅ Submits, EOVERFLOW |

### Key Findings
1. **Kernel limit identified**: Between 23,552 and 24,576 bytes total per URB
2. **Best achievable**: 2944 bytes/packet (57.7% of required 5104 bytes)
3. **Initial burst pattern**: Captures exactly **32-36 samples** (~9KB, 36ms of audio)
4. **Then complete failure**: All subsequent packets return EOVERFLOW with actual_length=0
5. **Multi-URB reaping working perfectly**: Processing thousands of URBs/sec, but all contain overflow errors

### Error Codes Encountered
- **errno 90 (EMSGSIZE)**: URB buffer too large for kernel to accept
- **status -75 (EOVERFLOW)**: Device sent more data than packet buffer can hold
- **status -71 (EPROTO)**: Protocol error (with 1024-byte packets, likely size mismatch)

---

## Captured Data Analysis

### Typical 5-Second Recording Results
- **Expected**: ~60 MB (240,000 samples)
- **Actual**: 8-9 KB (32-36 samples)
- **Success rate**: 0.015% of expected data
- **Pattern**: Initial burst only, then overflow errors

### File Sizes Observed
- spcmic_recording_*.wav: 7.9-9.0 KB
- Contains valid WAV header + 32-36 sample frames
- Each sample frame: 252 bytes (84 channels × 3 bytes)
- Proves data format is correct when captured
- Proves initialization sequence works
- **Indicates device IS streaming data continuously**

---

## Code State

### Current Configuration (Last Test)
```cpp
const int NUM_URBS = 32;              // 32 URBs in flight
const int PACKETS_PER_URB = 8;        // 8 packets per URB
const int URB_BUFFER_SIZE = 23552;    // ~23.5KB (8 × 2944 bytes)
// Packet size: 2944 bytes (maximum before EMSGSIZE)
```

### Key Code Sections

#### URB Initialization (lines 427-552)
- Dynamically allocates URBs with iso_frame_desc arrays
- Sets `USBDEVFS_URB_ISO_ASAP` flag
- Configures 8 packet descriptors per URB
- Successfully submits all 32 URBs

#### Multi-URB Reaping Loop (lines 596-709)
- Reaps up to 32 URBs per call until EAGAIN
- Checks packet status for errors
- Accumulates data from multiple URBs
- Immediately resubmits URBs to keep queue full
- **Working perfectly** but receiving overflow errors

#### Initialization Sequence (lines 328-385)
- Step 1: setInterface(3, 0) - disable streaming
- Step 2: Configure 48kHz sample rate on endpoint 0x81
- Step 3: setInterface(3, 1) - enable 84-channel streaming
- **Confirmed working** - device streams after this

---

## Research Conducted

### Linux USB Audio Driver Analysis
Used github_repo tool to study `torvalds/linux` repository:
- **ua101.c**: Uses 1 packet per URB for high-quality audio
- **usx2y, cx231xx, em28xx**: Process packets individually, skip empty ones
- **Key insight**: Empty ISO packets are normal in USB audio
- **Standard pattern**: Check actual_length, skip if zero, always resubmit URBs

### Android USB Documentation Review
- Android USB Host API has **16KB limit** before Android P (API 28)
- Native usbdevfs has different limits (discovered to be ~23.5KB)
- No documented workarounds for large isochronous transfers
- Standard Android audio HAL not designed for 84-channel USB devices

---

## Root Cause Analysis

### Why the Device Works on Windows/Linux
1. **No kernel URB size restrictions** (or much higher limits)
2. Can accommodate full 5104-byte packets
3. Standard USB Audio Class 2.0 drivers handle it transparently
4. Proven by device working "out of the box" on these platforms

### Why It Fails on Android
1. **Android kernel has ~23.5KB URB limit**
2. With 8 packets/URB: max 2944 bytes/packet
3. Device hardwired to send 5104 bytes/packet
4. **Mismatch causes EOVERFLOW**
5. Kernel discards overflow packets entirely
6. No mechanism to request different packet size from device
7. Device firmware cannot be modified (class-compliant device)

### The Initial Burst Phenomenon
- **First 32-36 URBs capture data successfully** (~9KB)
- Represents exactly **36 milliseconds** of audio
- Then ALL subsequent URBs return EOVERFLOW
- **Theory**: Device initially sends smaller packets during sync, then switches to full 5104-byte packets
- Once at full packet size, our buffers overflow continuously

---

## Attempted Solutions (All Failed)

1. ✗ **Reduced packet size to 1024 bytes**: EPROTO error
2. ✗ **Increased packet size to 5104 bytes**: EMSGSIZE (kernel rejects)
3. ✗ **Tried 4096 bytes** (power of 2): EMSGSIZE
4. ✗ **Increased NUM_URBS** (4→16→32): Same overflow pattern
5. ✗ **Multiple packets per URB** (1→8): Still hits kernel limit
6. ✗ **Bisected to find exact limit** (2944 bytes): Still overflows
7. ✗ **Researched Linux drivers**: No applicable workarounds for Android
8. ✗ **Tried different URB patterns**: All hit same fundamental limit

---

## Technical Constraints Summary

### Cannot Change on Device Side
- ❌ Device maxPacketSize (5104 bytes) - hardware fixed
- ❌ Device alternate settings - only 2 available
- ❌ Device firmware - class-compliant, no custom modifications
- ❌ USB protocol - UAC 2.0 standard

### Cannot Change on Android Side
- ❌ Kernel URB size limit (~23.5KB) - OS limitation
- ❌ Android USB Host API - inadequate for multichannel
- ❌ USB Audio HAL - not designed for 84 channels
- ❌ Custom kernel - not practical for distribution

---

## Potential Paths Forward (Unverified)

### 1. Accept Partial Data with Extrapolation
- Capture whatever fits in 2944-byte packets
- Algorithmically reconstruct missing data
- **Risk**: May not be valid audio data, high error rate

### 2. Custom Android Kernel
- Modify kernel to increase URB buffer limit to 40KB+
- Requires rooted device
- Not distributable as general app
- **Blocker**: Not practical for end users

### 3. Different USB Stack
- Investigate libusb for Android
- External USB library with different limits
- **Unknown**: If libusb has same kernel constraints

### 4. Hardware Workaround
- USB hub or adapter that buffers/splits data
- Intermediary device to repackage packets
- **Cost**: Requires additional hardware

### 5. Alternative Device
- Use USB audio interface with fewer channels
- Device with configurable packet sizes
- **Blocker**: Defeats purpose of 84-channel requirement

### 6. Reduce Channel Count
- Configure device for fewer channels (if possible)
- Check if alternate settings support 42 or 21 channels
- **Unknown**: Device descriptors suggest only 84-channel mode

---

## Questions for Further Investigation

1. **Is there a way to query the device for additional alternate settings?**
   - Current scan shows only Alt 0 (disabled) and Alt 1 (84ch, 5104 bytes)
   - Could there be hidden alternate settings with fewer channels?

2. **Can we modify the device's streaming format via control transfers?**
   - UAC 2.0 allows format negotiation
   - Could we request smaller packet sizes dynamically?
   - Would require deeper UAC 2.0 protocol implementation

3. **Is the Android kernel limit configurable?**
   - Can it be adjusted via sysfs or boot parameters?
   - Is there a per-device override mechanism?

4. **Does the device support USB 3.0 SuperSpeed?**
   - SuperSpeed has different packet structure
   - Might allow different buffer handling
   - Current testing done over USB 2.0

5. **Can ALSA on Android handle this device?**
   - ALSA (Advanced Linux Sound Architecture) for Android
   - Might have different URB handling
   - Would require investigation of Android audio subsystem

6. **Is there a USB Audio Class 3.0 approach?**
   - UAC 3.0 has different data transfer modes
   - Check if device supports UAC 3.0
   - Android support unknown

7. **Could we use USB bulk transfers instead of isochronous?**
   - Would lose timing guarantees
   - Might avoid packet size limits
   - Would require device firmware support (unlikely for class-compliant device)

---

## Development Environment

### Build Configuration
- Android Gradle Plugin: 8.4.0
- compileSdk: 35
- Gradle: 8.4.0
- NDK: Latest (CMake-based build)
- Language: Kotlin (app), C++ (native)

### Device Information
- Model: Samsung SM-S938W
- Android Version: 16 (latest)
- Connection: Wireless ADB (10.0.0.2:35987)
- USB OTG: Supported

### Build Status
- ✅ All builds successful
- ✅ Native code compiles without errors
- ✅ App installs and runs on device
- ✅ USB permissions granted
- ❌ Audio capture fails due to overflow

---

## Files to Review

### Primary Code Files
1. **app/src/main/cpp/usb_audio_interface.cpp** (777 lines)
   - Main URB management and isochronous transfer logic
   - Lines 427-433: URB configuration constants
   - Lines 535-552: URB initialization
   - Lines 596-709: Multi-URB reaping loop (critical section)
   - Lines 328-385: Device initialization sequence

2. **app/src/main/cpp/multichannel_recorder.cpp** (293 lines)
   - Recording thread and audio data processing
   - Calls readAudioData() every ~1.15ms

3. **app/src/main/cpp/uac_protocol.cpp**
   - USB Audio Class 2.0 protocol implementation
   - Sample rate configuration
   - Descriptor parsing

### Build Files
- **build.gradle** (app and project level)
- **CMakeLists.txt** (native build configuration)
- **gradle.properties**

### Documentation
- **README.md** (project overview)
- **.github/copilot-instructions.md** (development checklist)

---

## Logs and Evidence

### Successful URB Submission Log Example
```
I USBAudioInterface: Initialized 32 ISO m_urbs: buffer_length=23552, packets=8, packet_length=2944
I USBAudioInterface: Successfully submitted all 32 URBs
I USBAudioInterface: Sample rate configured successfully: 48000Hz
```

### EOVERFLOW Error Pattern (Repeats for entire 5-second recording)
```
E USBAudioInterface: URB[0] packet[0] error: status=-75, actual=0
E USBAudioInterface: URB[0] packet[1] error: status=-75, actual=0
E USBAudioInterface: URB[0] packet[2] error: status=-75, actual=0
...
E USBAudioInterface: URB[31] packet[7] error: status=-75, actual=0
I USBAudioInterface: Reaped 2 URBs in single call (reap#900), total bytes=0
```

### Final Recording Result
```
I MultichannelRecorder: Recording stopped. Total samples: 32
File size: 8.0K
```

---

## Conclusion

The project has successfully implemented:
- Complete USB device initialization and configuration
- Proper URB-based isochronous transfer infrastructure  
- High-performance multi-URB reaping loop
- Comprehensive error detection and logging

However, it has encountered a **fundamental hardware/OS incompatibility**:
- Android's kernel cannot accept URBs large enough for the device's packet size
- The device cannot reduce its packet size (hardware limitation)
- This creates an insurmountable EOVERFLOW condition
- Only 0.015% of expected data is captured (initial 36ms burst)

**The core question**: Is there ANY way to work around Android's ~23.5KB URB buffer limit to accommodate a USB Audio Class 2.0 device that requires 5104-byte isochronous packets?

Without solving this fundamental incompatibility, the SPCMic device cannot be used for multichannel recording on Android via the current native USB approach.

---

## Request for Fresh Perspective

A different AI model with expertise in:
- **Linux kernel USB subsystem internals**
- **Android USB/audio architecture**
- **USB Audio Class 2.0 protocol details**
- **Low-level USB driver development**

May be able to identify:
1. Alternative approaches we haven't considered
2. Workarounds for the URB size limitation
3. Different APIs or libraries that could help
4. Creative solutions to the packet size mismatch
5. Whether this is truly impossible on Android as-is

---

**Last Updated:** October 1, 2025  
**Status:** Blocked by Android kernel URB size limitation  
**Data Capture:** 0.015% success rate (36ms of audio from 5 seconds)

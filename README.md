# spcmic 84-Channel Audio Recorder

An Android application for capturing the full 84-channel output of the spcmic array over USB-C and saving it as 24-bit multichannel WAV files. The app now includes adaptive sample-rate selection and a clipping indicator to simplify field use.

## Features

- **84-Channel Recording** – Streams and stores every channel from the spcmic array with deterministic channel order.
- **Sample-Rate Picker** – UI exposes the discrete or continuous rates reported by the connected device (e.g., 48 kHz and 96 kHz for spcmic). Requests are negotiated through the USB clock source.
- **24-bit Uncompressed WAV** – Files are written with the negotiated rate, 24-bit samples, and 84 interleaved channels.
- **Clipping Indicator** – Latched “Clip” pill lights up if any channel hits 0 dBFS during the take; tap Reset to clear.
- **USB-C / UAC2 Support** – Communicates directly with UAC-compliant hardware without Android’s AudioRecord pipeline.
- **Lean UI** – Primary controls (connect, sample rate, record, clip status) optimized for quick deployment. The legacy multi-channel level meter is still present in code and can be re-enabled for diagnostics.

## Technical Specifications

- **Sample Rates**: Any discrete/continuous rates advertised by the device (spcmic exposes 48 kHz & 96 kHz; additional rates may appear after future firmware updates).
- **Bit Depth**: 24-bit PCM
- **Channels**: 84 interleaved
- **Container**: RIFF/WAV with full channel count metadata
- **Connection**: USB Audio Class 2 (tested with spcmic via USB-C OTG)
- **Minimum Android Version**: API 29 (Android 10)

## Requirements

### Hardware
- Android device with USB-C port and USB Host support
- spcmic 84-channel microphone array
- USB-C OTG / host-capable cable
- High-speed storage (24-bit/48kHz/84ch ≈ 1 GB/minute)

### Software
- Android 10.0 (API level 29) or higher
- USB Host feature support
- Audio recording permissions

## Installation

1. Clone this repository (`git clone https://github.com/<you>/Android_spcmic_recorder.git`)
2. Open the project in Android Studio (Giraffe or newer)
3. Build and install the Debug APK on your Android device
4. On first launch, grant USB and audio recording permissions when prompted

## Usage

1. **Connect the spcmic** – Plug the array into the Android device via USB-C. Use the on-screen *Reconnect* button if Android races you to the claim.
2. **Launch the App** – “USB Audio Device Connected” confirms we hold the interface.
3. **Select Sample Rate** – Use the spinner to choose from the supported rates. The UI also shows the device-reported rate after negotiation.
4. **Check Clip Status** – The clipping pill starts green (“No clipping detected”). It latches red if any channel hits full-scale.
5. **Start Recording** – Tap **Start Recording** to begin capture. The second label shows the negotiated rate that will be embedded in the WAV file.
6. **Reset Clipping (Optional)** – Tap **Reset** to clear the clip latch during a take.
7. **Stop Recording** – Tap **Stop** to finish and flush the WAV header.

## File Storage

Recorded files are saved to:
```
/storage/emulated/0/Documents/spcmicRecorder/
```

Files are named with timestamp format:
```
spcmic_recording_YYYYMMDD_HHMMSS.wav
```

> **Note**: The channel count makes files large. A 10-minute take at 48 kHz is ~10 GB.

## Technical Notes

### USB Audio Behaviour
- The app bypasses Android’s `AudioRecord` stack and talks to the USB device directly. We parse descriptors, program the clock source, set interface alt settings, and submit isochronous URBs over JNI.
- The sample-rate picker lists only the rates advertised by the selected interface/alt setting. If a rate is absent (e.g. 44.1 kHz), the hardware likely reserves it for a different alt setting.
- If the device rejects a rate change, use the *Reconnect* button to re-enumerate and request again.

### Performance Considerations
- **Data rate**: 24-bit × 84 channels × 48 kHz ≈ 9.7 MB/s (~1 GB/minute). 96 kHz doubles the throughput.
- Use fast storage (UFS 3.x / external SSD) and ensure >20 GB free before long sessions.
- Monitor device thermals during extended recordings; consider active cooling for best stability.

### Storage Requirements (rule of thumb)
- **Per Minute @ 48 kHz**: ~1.0 GB
- **Per Minute @ 96 kHz**: ~2.0 GB
- **Per Hour @ 48 kHz**: ~60 GB

## Development

### Building from Source
```bash
git clone [repository-url]
cd Android_spcmic_recorder
./gradlew build
```

### Key Components
- `MainActivity.kt`: Main UI and app coordination
- `USBAudioRecorder.kt`: Core audio recording functionality
- `LevelMeterView.kt`: Custom view for 84-channel level display
- `MainViewModel.kt`: App state and data management
- `native-lib.cpp` / `usb_audio_interface.cpp`: JNI bridge and USB audio engine

### Architecture
- **MVVM Pattern**: Clean separation of UI, business logic, and data
- **Kotlin Coroutines**: Efficient async audio processing
- **Custom Views**: 84-channel meter (currently hidden by default, can be re-enabled for debugging)
- **USB Host API**: Direct USB device communication

## Permissions

The app requires the following permissions:
- `RECORD_AUDIO`: For audio recording functionality
- `WRITE_EXTERNAL_STORAGE`: For saving WAV files (Android 10 and below)
- `READ_MEDIA_AUDIO`: For accessing audio files (Android 13+)
- `WAKE_LOCK`: To prevent device sleep during recording
- USB Host permissions for audio device access

## Troubleshooting

### Common Issues

**USB Device Not Detected**
- Verify your Android device supports USB Host/OTG.
- Confirm the spcmic is receiving power and enumerates on other hosts.
- Tap the **Reconnect** button to reclaim interfaces if Android Audio captures them first.

**Sample Rate Change Rejected**
- Some hardware exposes different rates on different alternate settings. If a request fails, tap **Reconnect** and retry at a supported rate (the UI shows the negotiated value).

**Recording Quality Issues**
- Check the negotiated rate label; if it differs from your request, reconnect.
- Ensure adequate storage bandwidth and free space.
- Keep the device cool and avoid running heavy background apps.

**Large File Management**
- WAV files are huge; transfer them off-device promptly.
- Add `*.wav` to `.gitignore` to avoid committing them to version control.

## License

[Add your license information here]

## Contributing

[Add contribution guidelines here]

## Support

For technical support or questions about the spcmic hardware, please contact:
[Add contact information]

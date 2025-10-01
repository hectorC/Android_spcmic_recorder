# SPCMic 84-Channel Audio Recorder

An Android application for recording 84 channels of uncompressed audio from the SPCMic (microphone array) via USB-C connection.

## Features

- **84-Channel Recording**: Supports recording from all 84 channels of the SPCMic array
- **High Quality Audio**: Records at 48kHz sample rate with 24-bit depth
- **Uncompressed WAV**: Saves audio in uncompressed WAV format for maximum quality
- **Real-time Level Meters**: Visual display of audio levels for all 84 channels
- **USB Audio Support**: Direct connection via USB-C to Android devices
- **Professional Interface**: Clean, intuitive UI designed for professional recording

## Technical Specifications

- **Sample Rate**: 48 kHz
- **Bit Depth**: 24-bit
- **Channels**: 84 (multichannel)
- **Format**: Uncompressed WAV
- **Connection**: USB Audio Class compliant devices
- **Minimum Android Version**: API 29 (Android 10)

## Requirements

### Hardware
- Android device with USB-C port and USB Host support
- SPCMic 84-channel microphone array
- USB-C cable
- Sufficient storage space (24-bit/48kHz/84ch = ~1GB per minute)

### Software
- Android 10.0 (API level 29) or higher
- USB Host feature support
- Audio recording permissions

## Installation

1. Clone or download this repository
2. Open the project in Android Studio
3. Build and install the APK on your Android device
4. Grant necessary permissions when prompted

## Usage

1. **Connect the SPCMic**: Plug the SPCMic array into your Android device via USB-C
2. **Launch the App**: Open the SPCMic Recorder application
3. **Check Connection**: Verify that "USB Audio Device Connected" appears
4. **Monitor Levels**: View real-time level meters for all 84 channels
5. **Start Recording**: Tap "Start Recording" to begin capture
6. **Stop Recording**: Tap "Stop Recording" to end and save the file

## File Storage

Recorded files are saved to:
```
/storage/emulated/0/Documents/SPCMicRecorder/
```

Files are named with timestamp format:
```
spcmic_recording_YYYYMMDD_HHMMSS.wav
```

## Technical Notes

### USB Audio Limitations
- Android's standard AudioRecord API has limitations with multichannel USB audio
- This implementation provides a framework that can be extended with custom USB audio drivers
- For production use, consider integrating with specialized USB audio libraries

### Performance Considerations
- 84-channel recording at 24-bit/48kHz generates approximately 1GB of data per minute
- Ensure sufficient storage space and USB transfer speeds
- Monitor device temperature during extended recording sessions

### Storage Requirements
- **Per Minute**: ~1GB (84 ch × 48kHz × 24-bit × 60s)
- **Per Hour**: ~60GB
- **Recommended**: Use high-speed storage (UFS 3.0+) for optimal performance

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

### Architecture
- **MVVM Pattern**: Clean separation of UI, business logic, and data
- **Kotlin Coroutines**: Efficient async audio processing
- **Custom Views**: Optimized UI components for multichannel display
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
- Verify USB Host support on your Android device
- Check that the SPCMic is USB Audio Class compliant
- Try a different USB-C cable
- Restart the app and reconnect the device

**Recording Quality Issues**
- Ensure adequate storage space
- Close other audio applications
- Check USB connection stability
- Monitor device temperature

**Performance Issues**
- Clear storage space (recommended 10GB+ free)
- Close background applications
- Ensure USB 3.0+ connection speeds
- Consider using external storage

## License

[Add your license information here]

## Contributing

[Add contribution guidelines here]

## Support

For technical support or questions about the SPCMic hardware, please contact:
[Add contact information]
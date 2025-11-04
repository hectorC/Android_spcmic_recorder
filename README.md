**Note:** This app is currently overall functional but it could still be unstable or buggy. If you encounter any problems, please use the issues section to report them.

# spcmic Recorder & Playback

An Android app for capturing, reviewing and exporting the full 84-channel output of the spcmic array [https://spcmic.com/] over USB-C. The project pairs a native USB audio engine with a Kotlin MVVM UI and Material Design 3 styling.

**Note:** This app requires that the spcmic is flashed with the latest firmware v2.x that converts it into a USB Audio Class Compliant device. This firmware is currently accessible through the 0.9.1 beta version of the spcmic app [https://harpex.net/spcmicsoftware/0.9.1%20beta/]

## Current Highlights

- **84-channel USB-UAC2 capture** at 24-bit/48 kHz or 96 kHz with deterministic channel order and standard WAV metadata.
- **Adaptive sample-rate negotiation** with information that shows both the requested and negotiated rates after enumeration (see settings panel).
- **Monitoring mode (no passthrough audio)** allowing USB streaming and level observation before committing to disk, with long-press exit for monitoring-only checks.
- **Real-time level metering (post-gain)** tracking the loudest channel across all 84 inputs, with visual feedback across green/yellow/red zones and a latched clipping indicator that persists until manually cleared.
- **Output to recording gain control** (0 to +48 dB) applied in the native capture pipeline before writing to disk, with clip detection for gain-induced overflows.
- **Large-file support** by promoting recordings to RF64 once they exceed RIFF limits (4GB) while keeping metadata aligned with the negotiated format.
- **Destination directory selection** in the settings panel, to either internal storage or external USB storage (e.g., SSD with a USB-C hub so the spcmic can be simultaneously plugged)
- **Native playback engine** plays a binaural decode (KU100) or previews the array by routing capsule 25 (left) and capsule 53 (right) directly to stereo, delivering instant playback alongside transport controls and a 0 to +48 dB playback only gain stage.
- **Loop toggle** that restarts playback at the end of the file.
- **Export workflow** decoupled from playback: start a binaural, ORTF, XY, or third-order Ambisonic export from any recording and watch progress in the processing overlay.
- **Dedicated exports directory** `Exports` created in the same directory as the recording target, keeping rendered mixes out of the source recording list.
- **Per-take geolocation** with an opt-in toggle, status indicator, and single-shot fixes stored in a `spcmic_locations.gpx` file stored in the recording target directory (accuracy, altitude, and provider metadata included).

## Requirements

### Hardware
- Android device running Android 10 (API 29) or newer with USB Host mode.
- spcmic 84-channel USB audio array with latest v2.x firmware.
- High-speed storage (rule of thumb: ~1 GB/minute at 48 kHz, ~2 GB/minute at 96 kHz).

### Software
- Android Studio Koala (2024.1.1) or newer with Android Gradle Plugin 8.6.1.
- JDK 17 (bundled with recent Android Studio releases).
- Gradle wrapper included in this repo (Gradle 8.8).

## Install
Pre-built apk files are available in the releases section (requires usual permissions in the device to allow for external apps to be installed)

## Building from source

Build debug version (with debug logging and symbols):
```powershell
cd d:\Audio_Projects\Android_spcmic_recorder
.\gradlew.bat assembleDebug
```

Build release version (optimized, smaller size):
```powershell
.\gradlew.bat assembleRelease
```

Build both versions:
```powershell
.\gradlew.bat assemble
```

**Output locations**:
- Debug APK: `app/build/outputs/apk/debug/app-debug.apk`
- Release APK: `app/build/outputs/apk/release/app-release.apk`

### Install to Connected Device

Install debug build (connect device via USB with debugging enabled):
```powershell
.\gradlew.bat installDebug
```

Install release build:
```powershell
.\gradlew.bat installRelease
```

Uninstall debug build:
```powershell
.\gradlew.bat uninstallDebug
```

Uninstall release build:
```powershell
.\gradlew.bat uninstallRelease
```

When connecting the spcmic, accept all the permissions, including the USB permission dialog and choose "Use by default" so the app is launched automatically the next time the spcmic is plugged in.

Use the refresh button (circular arrow) if the microphone is not detected (the sample rate display besides the refresh button should show the sample rate)

## Recording Workflow

1. **Connect hardware** – Plug the spcmic array into the device via USB-C. Tap *Reconnect* (circular arrow button) if Android claims the interface first or sample rate indicator is empty (you should see a toast alert when the device is successfully connected).
2. **Launch the recorder view** – The app auto-requests the default 48 kHz rate and displays both requested and negotiated values (see settings menu for configuration details).
3. **Select a sample rate** – Use the spinner to pick among the rates advertised by the interface/alt-setting (48 kHz, 96 kHz).
4. **Adjust gain** – Use the gain slider (0 to +48 dB) to boost input levels. Gain is applied in the native capture pipeline and written into the file. The level meter and clipping detector operate post-gain.
5. **(Optional) Enable geolocation** – Toggle the location switch in the settings panel to capture a single high-accuracy fix at the end of each take. Grant fine location permission when prompted. A location marker will appear in the top right corner of the timecode box when geolocation is enabled.
6. **Start monitoring** – Tap **START MONITORING** to begin USB streaming. The level meter shows the real-time peak level of the loudest channel, with color zones (green/yellow/red) indicating headroom. If any channel clips, a warning icon appears and the meter stays red until you tap to clear. There is no audio passthrough sent to the device's speakers or connected headphones during monitoring.
7. **Start recording** – Once levels look appropriate, tap **START RECORDING** to open a timestamped WAV file (e.g., `spcmic_recording_YYYYMMDD_HHMMSS.wav`) and begin writing audio to disk. The UI displays elapsed time.
8. **Stop recording** – Tap **STOP RECORDING** to finalize the file. Headers are back-filled with the negotiated format before the file is closed. When the payload exceeds 4 GB, the writer upgrades the container to RF64 and patches the ds64 chunk before close.
9. **Abort monitoring** – Long-press the button during monitoring to exit USB streaming without recording a file.

Recorded WAV files can be saved to:

- **Default location**: `/storage/emulated/0/Documents/spcmicRecorder/`
- **Custom location**: Use the "Storage Location" button in the Record tab to select any directory via Storage Access Framework (SAF)

The app supports both direct filesystem access and SAF (external storage) for maximum flexibility.

### Geolocation & GPX Sidecar

- Location capture is disabled by default. When enabled, the recorder requests a foreground single-shot fix from the Fused Location Provider when you stop recording.
- Per-take metadata (latitude/longitude, timestamp, accuracy, optional altitude, and provider string) is stored in `spcmic_locations.gpx` alongside the recording folder.
- Deleting a take from the playback list removes any matching `<wpt>` waypoint entry and deletes the gpx file when it becomes empty.

## Playback & Export Workflow

Playback preview routes capsule 25 to the left ear and capsule 53 to the right for instant stereo monitoring, while the convolution engine remains available for offline exports.

1. **Open a recording** – The playback screen lists top-level 84-channels recordings only; exports are hidden to avoid duplication.
2. **Adjust gain** – Drag the gain slider from 0 dB up to +48 dB while listening to the binaural decode (KU100) or the direct stereo pair (capsule 25 → left, capsule 53 → right). Binaural decode is enabled by default in the settings and can be disabled if binaural decode playback stutters due to low device performance (plays direct stereo pair instead).
3. **Toggle looping** – Use the loop button to restart playback at end of file.
4. **Export mixes** – Use the overflow menu to render Binaural, ORTF, XY, or third-order Ambisonic (16-channel) files. Completed exports land in `/Exports/` inisde the selected recording target directory.
5. **Delete recordings** – Use the overflow delete action to remove unwanted takes; confirmations prevent accidental loss.

## Architecture Notes

- **MVVM on the UI layer** with ViewModels, LiveData, and coroutines managing long-running tasks.
- **Three-state recorder** (IDLE → MONITORING → RECORDING) allowing pre-flight level checks without writing to disk, implemented via native C++ state machine.
- **Dual-thread recording pipeline** with lock-free ring buffer (4 MB) decoupling USB reads from disk writes to reduce the risk of dropouts under load.
- **Recording gain stage** applies 0 to +48 dB boost in the native pipeline (converted to linear multiplier) before samples reach the WAV writer, with overflow detection flagging gain-induced clipping.
- **64 URB queue** providing ~6-7 seconds of USB buffering plus ~3 seconds in the application ring buffer for resilience against system load.
- **Foreground service** with URGENT_AUDIO priority ensuring the recording process receives preferential CPU scheduling.
- **Native USB recording pipeline** built with C++ using Linux URBs (USB Request Blocks) for precise control over isochronous transfers and buffer management.
- **OpenSL ES playback engine** for low-latency direct stereo monitoring (capsules 25/53) plus the offline renderer that powers binaural, ORTF, XY, and 3OA exports.
- **Geolocation pipeline** that bridges the Fused Location Provider to the UI and GPX writer, capturing metadata exactly once per take and keeping the Kotlin/GPX layers in sync on deletes and reconnects.
- **JNI bridge** exposing playback transport, gain control, export hooks, and recording state transitions to Kotlin.
- **Material Design 3** theming with light/dark support and accessibility-focused controls.
- **Coroutines** orchestrating preprocessing, export rendering, and UI state updates without blocking the main thread.

## Known issues
- Sometimes the USB connection fails and audio will present crackling in all or some of the channels. This can be detected while monitoring in a quiet place at 0dB gain and observing the level meters. If the levels look abnormally loud (peaking mid-way in the meter) then stop monitoring, disconnect and reconnect the spcmic and test monitoring again 


## License & Contributions

MIT License

Copyright (c) 2025 Hector Centeno

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

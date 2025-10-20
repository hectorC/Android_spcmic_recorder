**Note (written by a human):** Contains AI generated code and functionality description text (with a bit of AI hyperbole). This app is currently functional overall but it could still be unstable or buggy. Use at your own risk.

# spcmic Recorder & Playback

An Android app for capturing and reviewing the full 84-channel output of the spcmic array over USB-C. The project pairs a native USB audio engine with a Kotlin MVVM UI and Material Design 3 styling to support field recordings plus binaural playback.

## Current Highlights

- **84-channel USB-UAC2 capture** at 24-bit/48 kHz (higher rates when exposed by the hardware) with deterministic channel order and standard WAV metadata.
- **Adaptive sample-rate negotiation** with a spinner that shows both the requested and negotiated rates after enumeration.
- **Monitoring mode** allowing USB streaming and level observation before committing to disk, with long-press exit for monitoring-only checks.
- **Real-time level metering** tracking the loudest channel across all 84 inputs, with visual feedback across green/yellow/red zones and a latched clipping indicator that persists until manually cleared.
- **Recording gain control** (0 to +48 dB) applied in the native capture pipeline before writing to disk, with clip detection for gain-induced overflows.
- **Large-file support** by promoting recordings to RF64 once they exceed RIFF limits while keeping metadata aligned with the negotiated format.
- **Native playback engine** with caching, transport controls, and a gain stage from 0 dB to +48 dB for the binaural preview path.
- **Loop toggle** that restarts playback at EOF while keeping button state and engine behavior in sync.
- **Export workflow** decoupled from playback: start an export from any recording, watch progress in the processing overlay, and leave playback ready the whole time.
- **Dedicated exports directory** at `/storage/emulated/0/Documents/spcmicRecorder/Exports/`, keeping rendered mixes out of the source recording list.
- **Per-take geolocation** with an opt-in toggle, status indicator, and single-shot fixes stored in a `spcmic_locations.gpx` sidecar (accuracy, altitude, and provider metadata included).
- **Overflow menu cleanup** with a working delete action, confirmation dialog, and automatic UI refresh.

## Requirements

### Hardware
- Android device running Android 10 (API 29) or newer with USB Host mode.
- spcmic 84-channel USB audio array with a USB-C OTG/host cable.
- High-speed storage (rule of thumb: ~1 GB/minute at 48 kHz, ~2 GB/minute at 96 kHz).
- Optional: external power or cooling for extended sessions.

### Software
- Android Studio Koala (2024.1.1) or newer with Android Gradle Plugin 8.6.1.
- JDK 17 (bundled with recent Android Studio releases).
- Gradle wrapper included in this repo (Gradle 8.8).

## Build & Install

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

When connecting the spcmic, accept the USB permission dialog and choose "Use by default" for smoother reconnects.

**Note**: The release build currently uses the debug keystore for signing. For production distribution, configure a proper signing key in `app/build.gradle`.

## Recording Workflow

1. **Connect hardware** – Plug the spcmic array into the device via USB-C. Tap *Reconnect* if Android claims the interface first.
2. **Launch the recorder view** – The app auto-requests the default 48 kHz rate and displays both requested and negotiated values (see settings menu).
3. **Select a sample rate** – Use the spinner to pick among the rates advertised by the interface/alt-setting (e.g., 48 kHz, 96 kHz).
4. **Adjust gain** – Use the gain slider (0 to +48 dB) to boost input levels. Gain is applied in the native capture pipeline and written into the file. The level meter and clipping detector operate post-gain.
5. **(Optional) Enable geolocation** – Toggle the location switch on the timecode card to capture a single high-accuracy fix at the end of each take. Grant fine location permission when prompted; the status pill updates from *Disabled* → *Searching* → *Locked*.
6. **Start monitoring** – Tap **START MONITORING** to begin USB streaming. The level meter shows the real-time peak level of the loudest channel, with color zones (green/yellow/red) indicating headroom. If any channel clips, a warning icon appears and the meter stays red until you tap to clear.
7. **Start recording** – Once levels look appropriate, tap **START RECORDING** to open a timestamped WAV file (e.g., `spcmic_recording_YYYYMMDD_HHMMSS.wav`) and begin writing audio to disk. The UI displays elapsed time.
8. **Stop recording** – Tap **STOP RECORDING** to finalize the file. Headers are back-filled with the negotiated format before the file is closed. When the payload exceeds 4 GB, the writer upgrades the container to RF64 and patches the ds64 chunk before close.
9. **Abort monitoring** – Long-press the button during monitoring to exit USB streaming without recording a file.

Recorded WAV files can be saved to:

- **Default location**: `/storage/emulated/0/Documents/spcmicRecorder/`
- **Custom location**: Use the "Storage Location" button in the Record tab to select any directory via Storage Access Framework (SAF)

The app supports both direct filesystem access and SAF for maximum flexibility.

### Geolocation & GPX Sidecar

- Location capture is disabled by default. When enabled, the recorder requests a foreground single-shot fix from the Fused Location Provider when you stop recording or monitoring.
- A status indicator on the timecode card reflects permission and fix state; a location icon appears when the take is tagged.
- Per-take metadata (latitude/longitude, timestamp, accuracy, optional altitude, and provider string) is persisted in `spcmic_locations.gpx` alongside the recording folder. Storage Access Framework targets receive the same GPX file via `DocumentsContract` writes.
- Deleting a take from the playback list removes any matching `<wpt>` entry (case-insensitive) and deletes the sidecar when it becomes empty.

## Playback & Export Workflow

1. **Open a recording** – The playback screen lists top-level recordings only; exports are hidden to avoid duplication.
2. **Adjust gain** – Drag the gain slider from 0 dB up to +48 dB for monitoring the binaural mix. Updates apply immediately through JNI to the native engine.
3. **Toggle looping** – Use the loop button to restart playback at EOF. State persists across configuration changes and mirrors the native engine.
4. **Watch the processing overlay** – Preprocessing or export operations surface progress in the shared overlay so you can track background work.
5. **Export binaural audio** – Choose *Export* from the overflow menu to render a stereo mix while playback remains ready. Completed exports land in `/Documents/spcmicRecorder/Exports/`.
6. **Delete recordings** – Use the overflow delete action to remove unwanted takes; confirmations prevent accidental loss.

## File Layout

- `app/src/main/java/com/spcmic/recorder/RecordFragment.kt` – Recording UI and controls.
- `app/src/main/java/com/spcmic/recorder/playback/PlaybackFragment.kt` – Playback UI, gain slider, loop toggle, export menu, and delete dialog.
- `app/src/main/java/com/spcmic/recorder/playback/PlaybackViewModel.kt` – LiveData state for playback, looping, gain, exports, and the processing overlay.
- `app/src/main/java/com/spcmic/recorder/playback/NativePlaybackEngine.kt` – Kotlin interface to the native engine via JNI.
- `app/src/main/java/com/spcmic/recorder/location/` – Opt-in geolocation toggle, preferences, single-shot capture manager, and GPX persistence utilities.
- `app/src/main/cpp/multichannel_recorder.cpp` – USB capture threads, ring buffer coordination, and clip detection.
- `app/src/main/cpp/wav_writer.cpp` – WAV/RF64 writer that back-fills headers for large files.
- `app/src/main/cpp/playback/` – C++ audio engine handling caching, gain staging, looping, and export rendering.
- `app/src/main/res/layout/` – Material Design 3 layouts for recording and playback surfaces.

## Architecture Notes

- **MVVM on the UI layer** with ViewModels, LiveData, and coroutines managing long-running tasks.
- **Three-state recorder** (IDLE → MONITORING → RECORDING) allowing pre-flight level checks without writing to disk, implemented via native C++ state machine.
- **Dual-thread recording pipeline** with lock-free ring buffer (4 MB) decoupling USB reads from disk writes to reduce the risk of dropouts under load.
- **Recording gain stage** applies 0 to +48 dB boost in the native pipeline (converted to linear multiplier) before samples reach the WAV writer, with overflow detection flagging gain-induced clipping.
- **64 URB queue** providing ~6-7 seconds of USB buffering plus ~3 seconds in the application ring buffer for resilience against system load.
- **Foreground service** with URGENT_AUDIO priority ensuring the recording process receives preferential CPU scheduling.
- **Native USB recording pipeline** built with C++ using Linux URBs (USB Request Blocks) for precise control over isochronous transfers and buffer management.
- **OpenSL ES playback engine** for low-latency binaural monitoring with gain control and looping.
- **Geolocation pipeline** that bridges the Fused Location Provider to the UI and GPX writer, capturing metadata exactly once per take and keeping the Kotlin/GPX layers in sync on deletes and reconnects.
- **JNI bridge** exposing playback transport, gain control, export hooks, and recording state transitions to Kotlin.
- **Material Design 3** theming with light/dark support and accessibility-focused controls.
- **Coroutines** orchestrating preprocessing, export rendering, and UI state updates without blocking the main thread.

## Troubleshooting

| Issue | Suggested Fix |
| --- | --- |
| **Device not detected** | Confirm USB Host support, power the spcmic externally if needed, and tap *Reconnect* after granting permission. |
| **Sample-rate request fails** | Some rates live on alternate USB interface settings. Reconnect and retry with a rate listed in the spinner. |
| **Level meter stays red** | Tap the level meter card to clear the latched clipping indicator. If clipping persists, reduce source gain or mic positioning. |
| **Audio dropouts during recording** | Ensure sufficient free storage space, avoid heavy multitasking during recording. The app maintains more than 10 seconds of buffering to mitigate dropouts. |
| **Large files** | At 48 kHz/84 ch expect ~1 GB per minute. The recorder switches to RF64 automatically once the data chunk approaches 4 GB. Move files off-device promptly and ensure ≥20 GB free before long sessions. |
| **Exports missing** | Check `/Documents/spcmicRecorder/Exports/` (not the recordings directory). |

## Roadmap

- Expand export presets beyond the current binaural mix.
- Add per-channel metering visualizations for detailed diagnostics.

## License & Contributions

The project currently carries no formal license or contribution guidelines. Coordinate with the maintainers before redistributing binaries or submitting patches.

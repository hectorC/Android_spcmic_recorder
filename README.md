**Note (written by a human):** Contains AI generated code and functionality description text (with a bit of AI hyperbole). This app is currently functional overall but it might be still unstable and contain buggs. Use at your own risk.

# spcmic Recorder & Playback

An Android app for capturing and reviewing the full 84-channel output of the spcmic array over USB-C. The project pairs a native USB audio engine with a Kotlin MVVM UI and Material Design 3 styling to support field recordings plus binaural playback.

## Current Highlights

- **84-channel USB-UAC2 capture** at 24-bit/48 kHz (higher rates when exposed by the hardware) with deterministic channel order and proper RIFF metadata.
- **Adaptive sample-rate negotiation** with a spinner that surfaces both the requested and negotiated rates after enumeration.
- **Latched clipping indicator** so the user can see when any channel hits 0 dBFS and clear it manually during a take.
- **Real-time playback engine** with caching, transport controls, and a gain stage that ranges from 0 dB to +48 dB for binaural preview.
- **Loop toggle** that restarts playback seamlessly at EOF while keeping button state and engine behavior in sync.
- **Export workflow** decoupled from playback: start an export from any recording, watch progress in the processing overlay, and leave playback ready the whole time.
- **Dedicated exports directory** at `/storage/emulated/0/Documents/spcmicRecorder/Exports/`, keeping rendered mixes out of the source recording list.
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

```powershell
cd d:\Audio_Projects\Android_spcmic_recorder
.\gradlew.bat assembleDebug
```

Install the resulting APK from `app/build/outputs/apk/debug/` onto a USB-host capable device. When connecting the spcmic, accept the USB permission dialog and choose "Use by default" for smoother reconnects.

## Recording Workflow

1. **Connect hardware** – Plug the spcmic array into the device via USB-C. Tap *Reconnect* if Android claims the interface first.
2. **Launch the recorder view** – The app auto-requests the default 48 kHz rate and displays both requested and negotiated values.
3. **Select a sample rate** – Use the spinner to pick among the rates advertised by the interface/alt-setting (e.g., 48 kHz, 96 kHz).
4. **Arm and monitor** – The clipping pill starts green. It latches red if any channel reaches full-scale; tap **Reset** to clear.
5. **Record** – Tap **Start Recording**. The UI shows elapsed time and the destination filename (e.g., `spcmic_recording_YYYYMMDD_HHMMSS.wav`).
6. **Stop** – Tap **Stop Recording** to finalize the file. Headers are back-filled with the negotiated format before the file is closed.

Recorded WAV files live in:

```
/storage/emulated/0/Documents/spcmicRecorder/
```

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
- `app/src/main/cpp/playback/` – C++ audio engine handling caching, gain staging, looping, and export rendering.
- `app/src/main/res/layout/` – Material Design 3 layouts for recording and playback surfaces.

## Architecture Notes

- **MVVM on the UI layer** with ViewModels, LiveData, and coroutines managing long-running tasks.
- **Native USB pipeline** built with C++ and OpenSL ES for precise control over isochronous transfers and buffer timing.
- **JNI bridge** exposing playback transport, gain control, and export hooks to Kotlin.
- **Material Design 3** theming with light/dark support and accessibility-focused controls.
- **Coroutines** orchestrating preprocessing, export rendering, and UI state updates without blocking the main thread.

## Troubleshooting

| Issue | Suggested Fix |
| --- | --- |
| **Device not detected** | Confirm USB Host support, power the spcmic externally if needed, and tap *Reconnect* after granting permission. |
| **Sample-rate request fails** | Some rates live on alternate USB interface settings. Reconnect and retry with a rate listed in the spinner. |
| **Clipping latch stays red** | Stop the take, tap **Reset**, and restart recording. Investigate source gain if the issue repeats. |
| **Large files** | At 48 kHz/84 ch expect ~1 GB per minute. Move files off-device promptly and ensure ≥20 GB free before long sessions. |
| **Exports missing** | Check `/Documents/spcmicRecorder/Exports/` (not the recordings directory). |

## Roadmap

- Wire up advanced meter visualizations for live diagnostics.
- Expand export presets beyond the current binaural mix.

## License & Contributions

The project currently carries no formal license or contribution guidelines. Coordinate with the maintainers before redistributing binaries or submitting patches.

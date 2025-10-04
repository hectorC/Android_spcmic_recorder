package com.spcmic.recorder.playback

/**
 * Enum representing available impulse response presets
 */
enum class IRPreset(val displayName: String, val fileName: String) {
    BINAURAL("Binaural", "binaural.wav"),
    ORTF("ORTF", "ortf.wav"),
    XY("XY Stereo", "xy.wav");
    
    companion object {
        fun getDisplayNames(): Array<String> {
            return values().map { it.displayName }.toTypedArray()
        }
        
        fun fromDisplayName(displayName: String): IRPreset? {
            return values().find { it.displayName == displayName }
        }
    }
}

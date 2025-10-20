package com.spcmic.recorder.playback

import androidx.annotation.StringRes
import com.spcmic.recorder.R

/**
 * Available export mix configurations.
 */
enum class ExportMixType(
    val presetId: Int,
    val outputChannels: Int,
    val cacheSuffix: String,
    val fileSuffix: String,
    @StringRes val labelResId: Int,
    val supportsCacheReuse: Boolean
) {
    BINAURAL(
        presetId = 0,
        outputChannels = 2,
        cacheSuffix = "",
        fileSuffix = "binaural",
        labelResId = R.string.mix_binaural,
        supportsCacheReuse = true
    ),
    ORTF(
        presetId = 1,
        outputChannels = 2,
        cacheSuffix = "ortf",
        fileSuffix = "ortf",
        labelResId = R.string.mix_ortf,
        supportsCacheReuse = false
    ),
    XY(
        presetId = 2,
        outputChannels = 2,
        cacheSuffix = "xy",
        fileSuffix = "xy",
        labelResId = R.string.mix_xy,
        supportsCacheReuse = false
    ),
    THIRD_ORDER_AMBISONIC(
        presetId = 3,
        outputChannels = 16,
        cacheSuffix = "3oa",
        fileSuffix = "3oa",
        labelResId = R.string.mix_3oa,
        supportsCacheReuse = false
    );

    val cacheFileName: String
        get() = if (cacheSuffix.isEmpty()) {
            "playback_cache.wav"
        } else {
            "playback_cache_${cacheSuffix}.wav"
        }
}

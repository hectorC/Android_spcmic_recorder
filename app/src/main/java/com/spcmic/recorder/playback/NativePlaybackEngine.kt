package com.spcmic.recorder.playback

import android.content.res.AssetManager

/**
 * Kotlin wrapper for native playback engine
 * Handles 84-channel WAV playback with stereo downmix
 */
class NativePlaybackEngine {

    enum class State {
        IDLE,
        PLAYING,
        PAUSED,
        STOPPED
    }

    private var engineHandle: Long = 0

    init {
        System.loadLibrary("spcmic_playback")
        engineHandle = nativeCreate()
    }

    /**
     * Load a 84-channel WAV file
     * @param filePath Absolute path to WAV file
     * @return true if loaded successfully
     */
    fun loadFile(filePath: String): Boolean {
        return nativeLoadFile(engineHandle, filePath)
    }

    fun setAssetManager(assetManager: AssetManager) {
        nativeSetAssetManager(engineHandle, assetManager)
    }

    fun setCacheDirectory(cacheDir: String) {
        nativeSetCacheDirectory(engineHandle, cacheDir)
    }

    fun useCachedPreRender(sourcePath: String): Boolean {
        return nativeUseCachedPreRender(engineHandle, sourcePath)
    }

    /**
     * Start playback
     */
    fun play(): Boolean {
        return nativePlay(engineHandle)
    }

    /**
     * Pause playback
     */
    fun pause() {
        nativePause(engineHandle)
    }

    /**
     * Stop playback and reset position
     */
    fun stop() {
        nativeStop(engineHandle)
    }

    /**
     * Seek to position
     * @param positionSeconds Position in seconds
     */
    fun seek(positionSeconds: Double): Boolean {
        return nativeSeek(engineHandle, positionSeconds)
    }

    /**
     * Get current playback state
     */
    fun getState(): State {
        val stateInt = nativeGetState(engineHandle)
        return State.values()[stateInt]
    }

    /**
     * Get current position in seconds
     */
    fun getPosition(): Double {
        return nativeGetPosition(engineHandle)
    }

    /**
     * Get total duration in seconds
     */
    fun getDuration(): Double {
        return nativeGetDuration(engineHandle)
    }

    /**
     * Check if file is loaded
     */
    fun isFileLoaded(): Boolean {
        return nativeIsFileLoaded(engineHandle)
    }

    fun preparePreRender(): Boolean {
        return nativePreparePreRender(engineHandle)
    }

    fun isPreRenderReady(): Boolean {
        return nativeIsPreRenderReady(engineHandle)
    }

    fun exportPreRendered(destinationPath: String): Boolean {
        return nativeExportPreRendered(engineHandle, destinationPath)
    }

    /**
     * Release native resources
     */
    fun release() {
        if (engineHandle != 0L) {
            nativeDestroy(engineHandle)
            engineHandle = 0
        }
    }

    protected fun finalize() {
        release()
    }

    // Native method declarations
    private external fun nativeCreate(): Long
    private external fun nativeDestroy(engineHandle: Long)
    private external fun nativeLoadFile(engineHandle: Long, filePath: String): Boolean
    private external fun nativePlay(engineHandle: Long): Boolean
    private external fun nativePause(engineHandle: Long)
    private external fun nativeStop(engineHandle: Long)
    private external fun nativeSeek(engineHandle: Long, positionSeconds: Double): Boolean
    private external fun nativeGetPosition(engineHandle: Long): Double
    private external fun nativeGetDuration(engineHandle: Long): Double
    private external fun nativeGetState(engineHandle: Long): Int
    private external fun nativeIsFileLoaded(engineHandle: Long): Boolean
    private external fun nativeSetAssetManager(engineHandle: Long, assetManager: AssetManager)
    private external fun nativeSetCacheDirectory(engineHandle: Long, cacheDir: String)
    private external fun nativeUseCachedPreRender(engineHandle: Long, sourcePath: String): Boolean
    private external fun nativePreparePreRender(engineHandle: Long): Boolean
    private external fun nativeIsPreRenderReady(engineHandle: Long): Boolean
    private external fun nativeExportPreRendered(engineHandle: Long, destinationPath: String): Boolean
}

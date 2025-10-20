#include <jni.h>
#include <string>
#include <android/log.h>
#include <android/asset_manager_jni.h>
#include "playback/playback_engine.h"
#include "../jni_probe.h"

#define LOG_TAG "PlaybackJNI"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

using namespace spcmic;

// Global playback engine instance
static PlaybackEngine* g_playbackEngine = nullptr;

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeCreate(
    JNIEnv* env,
    jobject /* this */) {
    LogJniProbe(env, "nativeCreate-entry", "PlaybackJNI");
    
    PlaybackEngine* engine = new PlaybackEngine();
    LOGD("Created playback engine: %p", engine);
    return reinterpret_cast<jlong>(engine);
}

JNIEXPORT void JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeDestroy(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativeDestroy-entry", "PlaybackJNI");
    
    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (engine) {
        LOGD("Destroying playback engine: %p", engine);
        delete engine;
    }
}

JNIEXPORT void JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeSetAssetManager(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jobject assetManager) {
    LogJniProbe(env, "nativeSetAssetManager-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle in setAssetManager");
        return;
    }

    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);
    engine->setAssetManager(mgr);
}

JNIEXPORT void JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeSetCacheDirectory(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jstring cacheDirectory) {
    LogJniProbe(env, "nativeSetCacheDirectory-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle in setCacheDirectory");
        return;
    }

    const char* dirChars = env->GetStringUTFChars(cacheDirectory, nullptr);
    std::string dir(dirChars);
    env->ReleaseStringUTFChars(cacheDirectory, dirChars);

    engine->setPreRenderCacheDirectory(dir);
}

JNIEXPORT void JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeConfigureExportPreset(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jint presetId,
    jint outputChannels,
    jstring cacheFileName) {
    LogJniProbe(env, "nativeConfigureExportPreset-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle in configureExportPreset");
        return;
    }

    IRPreset preset = IRPreset::Binaural;
    switch (presetId) {
        case static_cast<jint>(IRPreset::Binaural):
            preset = IRPreset::Binaural;
            break;
        case static_cast<jint>(IRPreset::Ortf):
            preset = IRPreset::Ortf;
            break;
        case static_cast<jint>(IRPreset::Xy):
            preset = IRPreset::Xy;
            break;
        case static_cast<jint>(IRPreset::ThirdOrderAmbisonic):
            preset = IRPreset::ThirdOrderAmbisonic;
            break;
        default:
            LOGW("Unknown preset id %d, defaulting to binaural", presetId);
            preset = IRPreset::Binaural;
            break;
    }

    std::string cacheName;
    if (cacheFileName != nullptr) {
        const char* chars = env->GetStringUTFChars(cacheFileName, nullptr);
        if (chars) {
            cacheName.assign(chars);
            env->ReleaseStringUTFChars(cacheFileName, chars);
        }
    }

    engine->configureExportPreset(preset, static_cast<int>(outputChannels), cacheName);
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeUseCachedPreRender(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jstring sourcePath) {
    LogJniProbe(env, "nativeUseCachedPreRender-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle in useCachedPreRender");
        return JNI_FALSE;
    }

    const char* pathChars = env->GetStringUTFChars(sourcePath, nullptr);
    std::string path(pathChars);
    env->ReleaseStringUTFChars(sourcePath, pathChars);

    bool success = engine->useExistingPreRendered(path);
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeLoadFile(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jstring filePath) {
    LogJniProbe(env, "nativeLoadFile-entry", "PlaybackJNI");
    
    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle");
        return JNI_FALSE;
    }

    const char* pathChars = env->GetStringUTFChars(filePath, nullptr);
    std::string path(pathChars);
    env->ReleaseStringUTFChars(filePath, pathChars);

    bool success = engine->loadFile(path);
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeLoadFileFromDescriptor(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jint fd,
    jstring displayPath) {
    LogJniProbe(env, "nativeLoadFileFromDescriptor-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle");
        return JNI_FALSE;
    }

    const char* pathChars = env->GetStringUTFChars(displayPath, nullptr);
    std::string path(pathChars ? pathChars : "descriptor");
    if (pathChars) {
        env->ReleaseStringUTFChars(displayPath, pathChars);
    }

    bool success = engine->loadFileFromDescriptor(fd, path);
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativePlay(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativePlay-entry", "PlaybackJNI");
    
    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle");
        return JNI_FALSE;
    }

    bool success = engine->play();
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativePause(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativePause-entry", "PlaybackJNI");
    
    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (engine) {
        engine->pause();
    }
}

JNIEXPORT void JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeStop(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativeStop-entry", "PlaybackJNI");
    
    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (engine) {
        engine->stop();
    }
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeSeek(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jdouble positionSeconds) {
    LogJniProbe(env, "nativeSeek-entry", "PlaybackJNI");
    
    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle");
        return JNI_FALSE;
    }

    bool success = engine->seek(positionSeconds);
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativePreparePreRender(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativePreparePreRender-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle");
        return JNI_FALSE;
    }

    bool success = engine->preparePreRenderedFile();
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeIsPreRenderReady(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativeIsPreRenderReady-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        return JNI_FALSE;
    }

    return engine->isPreRenderedReady() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeGetPreRenderProgress(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativeGetPreRenderProgress-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        return 0;
    }

    return static_cast<jint>(engine->getPreRenderProgress());
}

JNIEXPORT void JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeSetPlaybackGain(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jfloat gainDb) {
    LogJniProbe(env, "nativeSetPlaybackGain-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle in setPlaybackGain");
        return;
    }

    engine->setPlaybackGainDb(gainDb);
}

JNIEXPORT jfloat JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeGetPlaybackGain(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativeGetPlaybackGain-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        return 0.0f;
    }

    return engine->getPlaybackGainDb();
}

JNIEXPORT void JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeSetLooping(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jboolean enabled) {
    LogJniProbe(env, "nativeSetLooping-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle in setLooping");
        return;
    }

    engine->setLooping(enabled == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeIsLooping(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativeIsLooping-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        return JNI_FALSE;
    }

    return engine->isLooping() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeSetPlaybackConvolved(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jboolean enabled) {
    LogJniProbe(env, "nativeSetPlaybackConvolved-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle in setPlaybackConvolved");
        return;
    }

    engine->setPlaybackConvolved(enabled == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeIsPlaybackConvolved(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativeIsPlaybackConvolved-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        return JNI_FALSE;
    }

    return engine->isPlaybackConvolved() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeExportPreRendered(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jstring destinationPath) {
    LogJniProbe(env, "nativeExportPreRendered-entry", "PlaybackJNI");

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        LOGE("Invalid engine handle");
        return JNI_FALSE;
    }

    const char* destChars = env->GetStringUTFChars(destinationPath, nullptr);
    std::string dest(destChars);
    env->ReleaseStringUTFChars(destinationPath, destChars);

    bool success = engine->exportPreRenderedFile(dest);
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jdouble JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeGetPosition(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativeGetPosition-entry", "PlaybackJNI");
    
    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        return 0.0;
    }

    return engine->getPositionSeconds();
}

JNIEXPORT jdouble JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeGetDuration(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativeGetDuration-entry", "PlaybackJNI");
    
    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        return 0.0;
    }

    return engine->getDurationSeconds();
}

JNIEXPORT jint JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeGetState(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativeGetState-entry", "PlaybackJNI");
    
    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        return 0;  // IDLE
    }

    return static_cast<jint>(engine->getState());
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeIsFileLoaded(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    LogJniProbe(env, "nativeIsFileLoaded-entry", "PlaybackJNI");
    
    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        return JNI_FALSE;
    }

    return engine->isFileLoaded() ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"

#include <jni.h>
#include <string>
#include <android/log.h>
#include <android/asset_manager_jni.h>
#include "playback/playback_engine.h"

#define LOG_TAG "PlaybackJNI"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace spcmic;

// Global playback engine instance
static PlaybackEngine* g_playbackEngine = nullptr;

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeCreate(
    JNIEnv* env,
    jobject /* this */) {
    
    PlaybackEngine* engine = new PlaybackEngine();
    LOGD("Created playback engine: %p", engine);
    return reinterpret_cast<jlong>(engine);
}

JNIEXPORT void JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeDestroy(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle) {
    
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

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeUseCachedPreRender(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jstring sourcePath) {

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

    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        return JNI_FALSE;
    }

    return engine->isLooping() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_playback_NativePlaybackEngine_nativeExportPreRendered(
    JNIEnv* env,
    jobject /* this */,
    jlong engineHandle,
    jstring destinationPath) {

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
    
    PlaybackEngine* engine = reinterpret_cast<PlaybackEngine*>(engineHandle);
    if (!engine) {
        return JNI_FALSE;
    }

    return engine->isFileLoaded() ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"

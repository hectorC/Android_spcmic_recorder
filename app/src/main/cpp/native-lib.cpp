#include <jni.h>
#include <string>
#include <android/log.h>
#include "usb_audio_interface.h"
#include "multichannel_recorder.h"

#define LOG_TAG "SPCMicRecorder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static USBAudioInterface* g_usbAudioInterface = nullptr;
static MultichannelRecorder* g_recorder = nullptr;

extern "C" JNIEXPORT jstring JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    std::string hello = "SPCMic Native USB Audio Engine v1.0";
    return env->NewStringUTF(hello.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_initializeNativeAudio(
        JNIEnv* env,
        jobject thiz,
        jint deviceFd,
        jint sampleRate,
        jint channelCount) {
    
    LOGI("Initializing native USB audio with fd=%d, sampleRate=%d, channels=%d", 
         deviceFd, sampleRate, channelCount);
    
    try {
        if (g_usbAudioInterface) {
            delete g_usbAudioInterface;
        }
        
        g_usbAudioInterface = new USBAudioInterface();
        bool result = g_usbAudioInterface->initialize(deviceFd, sampleRate, channelCount);
        
        if (result) {
            if (g_recorder) {
                delete g_recorder;
            }
            g_recorder = new MultichannelRecorder(g_usbAudioInterface);
            LOGI("Native USB audio initialized successfully");
            return JNI_TRUE;
        } else {
            LOGE("Failed to initialize USB audio interface");
            return JNI_FALSE;
        }
    } catch (const std::exception& e) {
        LOGE("Exception in initializeNativeAudio: %s", e.what());
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_startRecordingNative(
        JNIEnv* env,
        jobject thiz,
        jstring outputPath) {
    
    if (!g_recorder) {
        LOGE("Recorder not initialized");
        return JNI_FALSE;
    }
    
    const char* pathStr = env->GetStringUTFChars(outputPath, nullptr);
    std::string path(pathStr);
    env->ReleaseStringUTFChars(outputPath, pathStr);
    
    LOGI("Starting native recording to: %s", path.c_str());
    
    bool result = g_recorder->startRecording(path);
    if (result) {
        LOGI("Native recording started successfully");
    } else {
        LOGE("Failed to start native recording");
    }
    
    return result ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_stopRecordingNative(
        JNIEnv* env,
        jobject thiz) {
    
    if (!g_recorder) {
        LOGE("Recorder not initialized");
        return JNI_FALSE;
    }
    
    LOGI("Stopping native recording");
    bool result = g_recorder->stopRecording();
    
    if (result) {
        LOGI("Native recording stopped successfully");
    } else {
        LOGE("Failed to stop native recording");
    }
    
    return result ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_getChannelLevelsNative(
        JNIEnv* env,
        jobject thiz) {
    
    if (!g_recorder) {
        return nullptr;
    }
    
    // Get 84-channel levels from USB Audio Class implementation
    std::vector<float> levels = g_recorder->getChannelLevels();
    
    jfloatArray result = env->NewFloatArray(levels.size());
    if (result) {
        env->SetFloatArrayRegion(result, 0, levels.size(), levels.data());
    }
    
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_startMonitoringNative(
        JNIEnv* env,
        jobject thiz) {
    
    if (!g_recorder) {
        LOGE("Recorder not initialized");
        return JNI_FALSE;
    }
    
    LOGI("Starting native USB Audio Class monitoring for 84-channel SPCMic");
    bool result = g_recorder->startMonitoring();
    
    if (result) {
        LOGI("Native USB Audio Class monitoring started successfully");
    } else {
        LOGE("Failed to start native USB Audio Class monitoring");
    }
    
    return result ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_stopMonitoringNative(
        JNIEnv* env,
        jobject thiz) {
    
    if (!g_recorder) {
        LOGE("Recorder not initialized");
        return;
    }
    
    LOGI("Stopping native USB Audio Class monitoring");
    g_recorder->stopMonitoring();
    LOGI("Native USB Audio Class monitoring stopped");
}

extern "C" JNIEXPORT void JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_releaseNativeAudio(
        JNIEnv* env,
        jobject thiz) {
    
    LOGI("Releasing native USB Audio Class resources");
    
    if (g_recorder) {
        g_recorder->stopRecording();
        g_recorder->stopMonitoring();
        delete g_recorder;
        g_recorder = nullptr;
    }
    
    if (g_usbAudioInterface) {
        g_usbAudioInterface->release();
        delete g_usbAudioInterface;
        g_usbAudioInterface = nullptr;
    }
    
    LOGI("Native USB Audio Class resources released");
}
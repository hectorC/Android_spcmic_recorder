#include <jni.h>
#include <string>
#include <vector>
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
Java_com_spcmic_recorder_USBAudioRecorder_startRecordingNativeWithFd(
        JNIEnv* env,
        jobject thiz,
        jint fd,
        jstring locationHint) {

    if (!g_recorder) {
        LOGE("Recorder not initialized");
        return JNI_FALSE;
    }

    std::string destinationLabel;
    if (locationHint != nullptr) {
        const char* hintChars = env->GetStringUTFChars(locationHint, nullptr);
        if (hintChars) {
            destinationLabel.assign(hintChars);
            env->ReleaseStringUTFChars(locationHint, hintChars);
        }
    }

    if (destinationLabel.empty()) {
        destinationLabel = "parcel_fd";
    }

    LOGI("Starting native recording via fd=%d (%s)", fd, destinationLabel.c_str());
    bool result = g_recorder->startRecordingWithFd(static_cast<int>(fd), destinationLabel);
    if (result) {
        LOGI("Native recording started successfully via fd=%d", fd);
    } else {
        LOGE("Failed to start native recording via fd=%d", fd);
    }
    return result ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_hasClippedNative(
        JNIEnv* env,
        jobject thiz) {

    if (!g_recorder) {
        return JNI_FALSE;
    }

    return g_recorder->hasClipped() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_resetClipIndicatorNative(
        JNIEnv* env,
        jobject thiz) {

    if (!g_recorder) {
        return;
    }

    g_recorder->resetClipIndicator();
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

extern "C" JNIEXPORT void JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_releaseNativeAudio(
        JNIEnv* env,
        jobject thiz) {
    
    LOGI("Releasing native USB Audio Class resources");
    
    if (g_recorder) {
        g_recorder->stopRecording();
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

extern "C" JNIEXPORT jintArray JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_getSupportedSampleRatesNative(
        JNIEnv* env,
        jobject thiz) {

    if (!g_usbAudioInterface) {
        return env->NewIntArray(0);
    }

    const std::vector<uint32_t>& rates = g_usbAudioInterface->getSupportedSampleRates();
    jintArray result = env->NewIntArray(static_cast<jsize>(rates.size()));
    if (!result) {
        return nullptr;
    }

    if (!rates.empty()) {
        std::vector<jint> temp;
        temp.reserve(rates.size());
        for (uint32_t rate : rates) {
            temp.push_back(static_cast<jint>(rate));
        }
        env->SetIntArrayRegion(result, 0, static_cast<jsize>(temp.size()), temp.data());
    }

    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_supportsContinuousSampleRateNative(
        JNIEnv* env,
        jobject thiz) {

    if (!g_usbAudioInterface) {
        return JNI_FALSE;
    }

    return g_usbAudioInterface->supportsContinuousSampleRate() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_getContinuousSampleRateRangeNative(
        JNIEnv* env,
        jobject thiz) {

    if (!g_usbAudioInterface || !g_usbAudioInterface->supportsContinuousSampleRate()) {
        return env->NewIntArray(0);
    }

    jintArray result = env->NewIntArray(2);
    if (!result) {
        return nullptr;
    }

    jint range[2];
    range[0] = static_cast<jint>(g_usbAudioInterface->getContinuousSampleRateMin());
    range[1] = static_cast<jint>(g_usbAudioInterface->getContinuousSampleRateMax());
    env->SetIntArrayRegion(result, 0, 2, range);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_getEffectiveSampleRateNative(
        JNIEnv* env,
        jobject thiz) {

    if (!g_usbAudioInterface) {
        return 0;
    }

    return static_cast<jint>(g_usbAudioInterface->getEffectiveSampleRateRounded());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_setTargetSampleRateNative(
        JNIEnv* env,
        jobject thiz,
        jint sampleRate) {

    if (!g_usbAudioInterface) {
        return JNI_FALSE;
    }

    bool result = g_usbAudioInterface->setTargetSampleRate(static_cast<int>(sampleRate));
    return result ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_setInterfaceNative(
        JNIEnv* env,
        jobject thiz,
        jint interfaceNum,
        jint altSetting) {

    if (!g_usbAudioInterface) {
        LOGE("Cannot set interface: USB audio interface not initialized");
        return JNI_FALSE;
    }

    LOGI("Setting USB interface %d to alt setting %d", interfaceNum, altSetting);
    bool result = g_usbAudioInterface->setInterface(static_cast<int>(interfaceNum), static_cast<int>(altSetting));
    
    if (result) {
        LOGI("Successfully set interface %d to alt %d", interfaceNum, altSetting);
    } else {
        LOGE("Failed to set interface %d to alt %d", interfaceNum, altSetting);
    }
    
    return result ? JNI_TRUE : JNI_FALSE;
}
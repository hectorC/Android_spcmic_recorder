#include <jni.h>
#include <string>
#include <vector>
#include <android/log.h>
#include <mutex>

#include "usb_audio_interface.h"
#include "multichannel_recorder.h"

#define LOG_TAG "SPCMicRecorder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// Single mutex to protect all access to global native pointers.
static std::mutex g_nativeMutex;
static USBAudioInterface* g_usbAudioInterface = nullptr;
static MultichannelRecorder* g_recorder = nullptr;
static JavaVM* g_javaVm = nullptr;


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
    std::lock_guard<std::mutex> lock(g_nativeMutex);
    
    LOGI("Initializing native USB audio with fd=%d, sampleRate=%d, channels=%d", 
         deviceFd, sampleRate, channelCount);
    
    try {
        if (g_usbAudioInterface) {
            LOGW("JNI", "Re-initializing native audio. Deleting previous interface.");
            delete g_usbAudioInterface;
            g_usbAudioInterface = nullptr;
        }
        
        g_usbAudioInterface = new USBAudioInterface();
        bool result = g_usbAudioInterface->initialize(deviceFd, sampleRate, channelCount);
        
        if (result) {
            LOGI("Native USB audio initialized successfully");
            return JNI_TRUE;
        } else {
            LOGE("Failed to initialize USB audio interface");
            delete g_usbAudioInterface;
            g_usbAudioInterface = nullptr;
            return JNI_FALSE;
        }
    } catch (const std::exception& e) {
        LOGE("Exception in initializeNativeAudio: %s", e.what());
        if (g_usbAudioInterface) {
            delete g_usbAudioInterface;
            g_usbAudioInterface = nullptr;
        }
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_startRecordingNative(
        JNIEnv* env,
        jobject thiz,
        jstring outputPath) {
    std::lock_guard<std::mutex> lock(g_nativeMutex);
    
    if (!g_usbAudioInterface) {
        LOGE("USB Audio Interface not initialized, cannot start recording");
        return JNI_FALSE;
    }

    // If a recorder instance exists, it must be a stale one from a previous, stopped session.
    // It is now safe to delete it before creating a new one.
    if (g_recorder) {
        LOGW("JNI", "Stale recorder instance found. Deleting it before starting new recording.");
        delete g_recorder;
        g_recorder = nullptr;
    }

    try {
        g_recorder = new MultichannelRecorder(g_usbAudioInterface);
        
        const char* pathStr = env->GetStringUTFChars(outputPath, nullptr);
        std::string path(pathStr);
        env->ReleaseStringUTFChars(outputPath, pathStr);
        
        LOGI("Starting native recording to: %s", path.c_str());
        
        bool result = g_recorder->startRecording(path);
        if (result) {
            LOGI("Native recording started successfully");
        } else {
            LOGE("Failed to start native recording");
            delete g_recorder;
            g_recorder = nullptr;
        }
        
        return result ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& e) {
        LOGE("Exception in startRecordingNative: %s", e.what());
        if (g_recorder) {
            delete g_recorder;
            g_recorder = nullptr;
        }
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_startRecordingNativeWithFd(
        JNIEnv* env,
        jobject thiz,
        jint fd,
        jstring locationHint) {
    std::lock_guard<std::mutex> lock(g_nativeMutex);

    if (!g_usbAudioInterface) {
        LOGE("USB Audio Interface not initialized, cannot start recording");
        return JNI_FALSE;
    }

    // If a recorder instance exists, it must be a stale one from a previous, stopped session.
    if (g_recorder) {
        LOGW("JNI", "Stale recorder instance found. Deleting it before starting new recording.");
        delete g_recorder;
        g_recorder = nullptr;
    }

    try {
        g_recorder = new MultichannelRecorder(g_usbAudioInterface);

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
            delete g_recorder;
            g_recorder = nullptr;
        }
        return result ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& e) {
        LOGE("Exception in startRecordingNativeWithFd: %s", e.what());
        if (g_recorder) {
            delete g_recorder;
            g_recorder = nullptr;
        }
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_hasClippedNative(
        JNIEnv* env,
        jobject thiz) {
    std::lock_guard<std::mutex> lock(g_nativeMutex);

    if (!g_recorder) {
        return JNI_FALSE;
    }

    return g_recorder->hasClipped() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_resetClipIndicatorNative(
        JNIEnv* env,
        jobject thiz) {
    std::lock_guard<std::mutex> lock(g_nativeMutex);

    if (g_recorder) {
        g_recorder->resetClipIndicator();
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_stopRecordingNative(
        JNIEnv* env,
        jobject thiz) {
    std::lock_guard<std::mutex> lock(g_nativeMutex);
    
    if (!g_recorder) {
        LOGW("JNI", "stopRecordingNative called but g_recorder is null.");
        return JNI_TRUE; // Already in the desired state.
    }
    
    LOGI("Stopping native recording...");
    bool result = g_recorder->stopRecording();
    
    if (result) {
        LOGI("Native recording stopped successfully. The recorder instance is now idle.");
    } else {
        LOGE("Failed to stop native recording cleanly.");
    }

    // We NO LONGER delete g_recorder here.
    // The instance is left in a "stopped" state. The next call to
    // startRecordingNative is responsible for cleaning up this old instance.
    
    return result ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_releaseNativeAudio(
        JNIEnv* env,
        jobject thiz) {
    std::lock_guard<std::mutex> lock(g_nativeMutex);
    
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
    std::lock_guard<std::mutex> lock(g_nativeMutex);

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
    std::lock_guard<std::mutex> lock(g_nativeMutex);

    if (!g_usbAudioInterface) {
        return JNI_FALSE;
    }

    return g_usbAudioInterface->supportsContinuousSampleRate() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_spcmic_recorder_USBAudioRecorder_getContinuousSampleRateRangeNative(
        JNIEnv* env,
        jobject thiz) {
    std::lock_guard<std::mutex> lock(g_nativeMutex);

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
    std::lock_guard<std::mutex> lock(g_nativeMutex);

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
    std::lock_guard<std::mutex> lock(g_nativeMutex);

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
    std::lock_guard<std::mutex> lock(g_nativeMutex);

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

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_javaVm = vm;
    return JNI_VERSION_1_6;
}
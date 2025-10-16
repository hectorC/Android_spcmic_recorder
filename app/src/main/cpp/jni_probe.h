#pragma once

#include <jni.h>
#include <android/log.h>
#include <cstring>

#if defined(ENABLE_ADDRESS_SANITIZER)
#if defined(__has_attribute)
#if __has_attribute(no_sanitize)
#ifndef NO_SANITIZE_ADDRESS
#define NO_SANITIZE_ADDRESS __attribute__((no_sanitize("address")))
#endif
#endif
#endif
#ifndef NO_SANITIZE_ADDRESS
#define NO_SANITIZE_ADDRESS
#endif

inline NO_SANITIZE_ADDRESS void LogJniProbe(JNIEnv* env, const char* stage,
                                            const char* tag = "SPCMicRecorder") {
    if (!env) {
        __android_log_print(ANDROID_LOG_ERROR, tag,
                            "JNI probe '%s': env is null", stage ? stage : "<unknown>");
        return;
    }

    __android_log_print(ANDROID_LOG_INFO, tag, "JNI probe '%s': env=%p",
                        stage ? stage : "<unknown>", static_cast<void*>(env));

    void* tablePtrRaw = nullptr;
    std::memcpy(&tablePtrRaw, env, sizeof(void*));
    __android_log_print(ANDROID_LOG_INFO, tag, "JNI probe '%s': vtable=%p",
                        stage ? stage : "<unknown>", tablePtrRaw);

    if (!tablePtrRaw) {
        __android_log_print(ANDROID_LOG_ERROR, tag,
                            "JNI probe '%s': vtable null; skipping entry dump",
                            stage ? stage : "<unknown>");
        return;
    }

    auto tablePtr = static_cast<void**>(tablePtrRaw);
    if (!tablePtr) {
        __android_log_print(ANDROID_LOG_ERROR, tag,
                            "JNI probe '%s': vtable pointer null; aborting entry dump",
                            stage ? stage : "<unknown>");
        return;
    }

    void* entries[8] = {nullptr};
    std::memcpy(entries, tablePtr, sizeof(entries));

    __android_log_print(ANDROID_LOG_INFO, tag,
                        "JNI probe '%s': vtable entries -> [0]=%p [1]=%p [2]=%p [3]=%p [4]=%p [5]=%p [6]=%p [7]=%p",
                        stage ? stage : "<unknown>", entries[0], entries[1], entries[2], entries[3],
                        entries[4], entries[5], entries[6], entries[7]);
}

inline NO_SANITIZE_ADDRESS void LogJavaVmProbe(JavaVM* vm, const char* stage,
                                               const char* tag = "SPCMicRecorder") {
    if (!vm) {
        __android_log_print(ANDROID_LOG_ERROR, tag,
                            "JavaVM probe '%s': vm is null", stage ? stage : "<unknown>");
        return;
    }

    __android_log_print(ANDROID_LOG_INFO, tag, "JavaVM probe '%s': vm=%p",
                        stage ? stage : "<unknown>", static_cast<void*>(vm));

    void* tablePtrRaw = nullptr;
    std::memcpy(&tablePtrRaw, vm, sizeof(void*));
    __android_log_print(ANDROID_LOG_INFO, tag, "JavaVM probe '%s': vtable=%p",
                        stage ? stage : "<unknown>", tablePtrRaw);

    if (!tablePtrRaw) {
        __android_log_print(ANDROID_LOG_ERROR, tag,
                            "JavaVM probe '%s': vtable null; skipping entry dump",
                            stage ? stage : "<unknown>");
        return;
    }

    auto tablePtr = static_cast<void**>(tablePtrRaw);
    if (!tablePtr) {
        __android_log_print(ANDROID_LOG_ERROR, tag,
                            "JavaVM probe '%s': vtable pointer null; aborting entry dump",
                            stage ? stage : "<unknown>");
        return;
    }

    void* entries[8] = {nullptr};
    std::memcpy(entries, tablePtr, sizeof(entries));

    __android_log_print(ANDROID_LOG_INFO, tag,
                        "JavaVM probe '%s': vtable entries -> [0]=%p [1]=%p [2]=%p [3]=%p [4]=%p [5]=%p [6]=%p [7]=%p",
                        stage ? stage : "<unknown>", entries[0], entries[1], entries[2], entries[3],
                        entries[4], entries[5], entries[6], entries[7]);
}
#else
inline void LogJniProbe(JNIEnv*, const char*, const char* = "SPCMicRecorder") {}
inline void LogJavaVmProbe(JavaVM*, const char*, const char* = "SPCMicRecorder") {}
#endif

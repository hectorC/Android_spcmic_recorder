# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# If your project uses WebView with JS, uncomment the following
# and specify the fully qualified class name to the JavaScript interface
# class:
#-keepclassmembers class fqcn.of.javascript.interface.for.webview {
#   public *;
#}

# ============================================================================
# Crash Reporting - Keep debug info for readable stack traces
# ============================================================================
-keepattributes SourceFile,LineNumberTable
-keepattributes *Annotation*,Signature,Exception
-renamesourcefileattribute SourceFile

# ============================================================================
# Remove Logging - Strip all Log calls except errors
# ============================================================================
# Remove verbose logging
-assumenosideeffects class android.util.Log {
    public static int v(...);
    public static int d(...);
    public static int i(...);
    public static int w(...);
}

# Keep error logging for production debugging
# Comment out the next line if you want to remove ALL logging including errors
-assumenosideeffects class android.util.Log {
    public static int e(...);
}

# ============================================================================
# JNI - Keep all native methods
# ============================================================================
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep all classes with native methods (USBAudioRecorder, etc.)
-keep class com.spcmic.recorder.USBAudioRecorder {
    native <methods>;
    public <init>(...);
}

-keep class com.spcmic.recorder.playback.PlaybackEngine {
    native <methods>;
    public <init>(...);
}

# ============================================================================
# USB and Android Services - Prevent obfuscation
# ============================================================================
# Keep USB-related classes that Android system interacts with
-keep class * extends android.app.Service
-keep class * extends android.content.BroadcastReceiver
-keep class * implements android.os.Parcelable {
    public static final android.os.Parcelable$Creator *;
}

# Keep AudioRecordingService
-keep class com.spcmic.recorder.AudioRecordingService {
    public <methods>;
}

# ============================================================================
# ViewBinding - Keep generated binding classes
# ============================================================================
-keep class * implements androidx.viewbinding.ViewBinding {
    public static *** bind(android.view.View);
    public static *** inflate(android.view.LayoutInflater);
}

# ============================================================================
# ViewModel and LiveData - Keep for proper lifecycle
# ============================================================================
-keep class * extends androidx.lifecycle.ViewModel {
    <init>(...);
}

-keep class * extends androidx.lifecycle.AndroidViewModel {
    <init>(...);
}

# ============================================================================
# Kotlin - Standard rules
# ============================================================================
-keep class kotlin.Metadata { *; }
-keepclassmembers class kotlin.Metadata {
    public <methods>;
}

# Keep Kotlin coroutines
-keepnames class kotlinx.coroutines.internal.MainDispatcherFactory {}
-keepnames class kotlinx.coroutines.CoroutineExceptionHandler {}

# ============================================================================
# Google Play Services - Location
# ============================================================================
-keep class com.google.android.gms.** { *; }
-dontwarn com.google.android.gms.**

# Keep location-related classes
-keep class com.spcmic.recorder.location.** {
    public <methods>;
    public <fields>;
}

# ============================================================================
# Data Classes and Serialization
# ============================================================================
-keepclassmembers class * {
    @kotlinx.serialization.Serializable <fields>;
}

# Keep data class copy methods
-keepclassmembers class * {
    public ** copy(...);
}

# ============================================================================
# Enum optimization
# ============================================================================
-keepclassmembers enum * {
    public static **[] values();
    public static ** valueOf(java.lang.String);
}

# ============================================================================
# General Android Framework
# ============================================================================
-keepclassmembers class * extends android.app.Activity {
    public void *(android.view.View);
}

-keepclassmembers class * extends androidx.fragment.app.Fragment {
    public void *(android.view.View);
}

# Keep custom view constructors
-keepclasseswithmembers class * {
    public <init>(android.content.Context, android.util.AttributeSet);
}

-keepclasseswithmembers class * {
    public <init>(android.content.Context, android.util.AttributeSet, int);
}

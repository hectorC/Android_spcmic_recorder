package com.spcmic.recorder

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.Process
import androidx.core.app.NotificationCompat

/**
 * Foreground service for high-priority audio recording.
 * Running as a foreground service ensures the system won't kill the app during recording
 * and gives the process higher scheduling priority.
 */
class AudioRecordingService : Service() {
    
    private val binder = AudioRecordingBinder()
    private var isRecording = false
    
    inner class AudioRecordingBinder : Binder() {
        fun getService(): AudioRecordingService = this@AudioRecordingService
    }
    
    override fun onBind(intent: Intent): IBinder {
        return binder
    }
    
    override fun onCreate() {
        super.onCreate()
        
        // Increase process priority for audio recording
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            android.util.Log.i("AudioRecordingService", "Set thread priority to URGENT_AUDIO")
        } catch (e: Exception) {
            android.util.Log.e("AudioRecordingService", "Failed to set thread priority: ${e.message}")
        }
    }
    
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START_RECORDING -> startRecording()
            ACTION_STOP_RECORDING -> stopRecording()
        }
        
        // Ensure service restarts if killed by the system during recording
        return START_STICKY
    }
    
    private fun startRecording() {
        if (isRecording) return
        
        isRecording = true
        
        // Create notification channel for Android O and above
        createNotificationChannel()
        
        // Create notification
        val notification = createNotification()
        
        // Start as foreground service - this gives us high priority
        startForeground(NOTIFICATION_ID, notification)
        
        android.util.Log.i("AudioRecordingService", "Started foreground service for recording")
    }
    
    private fun stopRecording() {
        isRecording = false
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
        
        android.util.Log.i("AudioRecordingService", "Stopped foreground service")
    }
    
    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Audio Recording",
                NotificationManager.IMPORTANCE_LOW // Low importance = no sound/vibration
            ).apply {
                description = "Recording 84-channel audio from spcmic"
                setShowBadge(false)
                lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            }
            
            val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            notificationManager.createNotificationChannel(channel)
        }
    }
    
    private fun createNotification(): Notification {
        // Intent to open MainActivity when notification is tapped
        val notificationIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK
        }
        
        val pendingIntent = PendingIntent.getActivity(
            this,
            0,
            notificationIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("spcmic Recording")
            .setContentText("Recording 84-channel audio...")
            .setSmallIcon(R.drawable.ic_microphone)
            .setContentIntent(pendingIntent)
            .setOngoing(true) // Prevents user from dismissing
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .build()
    }
    
    companion object {
        private const val NOTIFICATION_ID = 1001
        private const val CHANNEL_ID = "audio_recording_channel"
        
        const val ACTION_START_RECORDING = "com.spcmic.recorder.START_RECORDING"
        const val ACTION_STOP_RECORDING = "com.spcmic.recorder.STOP_RECORDING"
        
        fun startRecordingService(context: Context) {
            val intent = Intent(context, AudioRecordingService::class.java).apply {
                action = ACTION_START_RECORDING
            }
            
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }
        
        fun stopRecordingService(context: Context) {
            val intent = Intent(context, AudioRecordingService::class.java).apply {
                action = ACTION_STOP_RECORDING
            }
            context.startService(intent)
        }
    }
}

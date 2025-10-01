package com.spcmic.recorder

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Bundle
import android.util.Log
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.ViewModelProvider
import com.spcmic.recorder.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {
    
    private lateinit var binding: ActivityMainBinding
    private lateinit var viewModel: MainViewModel
    private lateinit var usbManager: UsbManager
    private lateinit var audioRecorder: USBAudioRecorder
    
    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val allGranted = permissions.all { it.value }
        if (allGranted) {
            initializeAudioRecorder()
        } else {
            Toast.makeText(this, "Audio recording permission is required", Toast.LENGTH_LONG).show()
        }
    }
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        
        viewModel = ViewModelProvider(this)[MainViewModel::class.java]
        usbManager = getSystemService(USB_SERVICE) as UsbManager
        
        setupUI()
        checkPermissions()
        observeViewModel()
        
        // Handle USB device attachment if app was launched by USB intent
        handleUsbIntent(intent)
    }
    
    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        intent?.let { handleUsbIntent(it) }
    }
    
    private fun handleUsbIntent(intent: Intent) {
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED == intent.action) {
            val usbDevice: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            usbDevice?.let { device ->
                Log.i("MainActivity", "USB device attached: ${device.deviceName} (${device.manufacturerName} ${device.productName})")
                
                // Check if this is our target SPCMic device
                if (device.vendorId == 22564 && device.productId == 10208) {
                    Log.i("MainActivity", "SPCMic device detected - claiming device immediately")
                    
                    // Claim the device immediately to prevent Android audio system from taking it
                    claimUsbDevice(device)
                } else {
                    Log.i("MainActivity", "Generic USB audio device detected")
                    // Handle other USB audio devices if needed
                    refreshUSBDevices()
                }
            }
        }
    }
    
    private fun claimUsbDevice(device: UsbDevice) {
        try {
            // Initialize audio recorder if not already done
            if (!::audioRecorder.isInitialized) {
                audioRecorder = USBAudioRecorder(this, viewModel)
            }
            
            // Connect to the device immediately
            val success = audioRecorder.connectToDevice(device)
            if (success) {
                viewModel.setUSBDevice(device)
                Toast.makeText(this, "SPCMic claimed successfully!", Toast.LENGTH_SHORT).show()
                Log.i("MainActivity", "Successfully claimed SPCMic device")
            } else {
                Log.e("MainActivity", "Failed to claim SPCMic device")
                Toast.makeText(this, "Failed to claim SPCMic device", Toast.LENGTH_SHORT).show()
            }
        } catch (e: Exception) {
            Log.e("MainActivity", "Exception while claiming USB device", e)
            Toast.makeText(this, "Error claiming USB device: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }
    
    private fun setupUI() {
        binding.apply {
            btnRecord.setOnClickListener {
                if (viewModel.isRecording.value == true) {
                    stopRecording()
                } else {
                    startRecording()
                }
            }
            
            btnRefreshDevices.setOnClickListener {
                refreshUSBDevices()
            }
        }
    }
    
    private fun observeViewModel() {
        viewModel.isRecording.observe(this) { isRecording ->
            binding.btnRecord.text = if (isRecording) "Stop Recording" else "Start Recording"
            binding.btnRecord.isEnabled = viewModel.isUSBDeviceConnected.value == true
        }
        
        viewModel.isUSBDeviceConnected.observe(this) { isConnected ->
            binding.tvConnectionStatus.text = if (isConnected) {
                "USB Audio Device Connected"
            } else {
                "No USB Audio Device Found"
            }
            binding.btnRecord.isEnabled = isConnected && !viewModel.isRecording.value!!
        }
        
        viewModel.recordingTime.observe(this) { time ->
            binding.tvRecordingTime.text = formatTime(time)
        }
        
        viewModel.channelLevels.observe(this) { levels ->
            binding.levelMeterView.updateLevels(levels)
        }
    }
    
    private fun checkPermissions() {
        val permissionsNeeded = mutableListOf<String>()
        
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) 
            != PackageManager.PERMISSION_GRANTED) {
            permissionsNeeded.add(Manifest.permission.RECORD_AUDIO)
        }
        
        if (permissionsNeeded.isNotEmpty()) {
            requestPermissionLauncher.launch(permissionsNeeded.toTypedArray())
        } else {
            initializeAudioRecorder()
        }
    }
    
    private fun initializeAudioRecorder() {
        audioRecorder = USBAudioRecorder(this, viewModel)
        // Don't auto-connect - wait for explicit USB device attachment or user action
        updateUSBDeviceList()
    }
    
    private fun updateUSBDeviceList() {
        // Just update the list without connecting
        val deviceList = usbManager.deviceList
        val audioDevices = deviceList.values.filter { device ->
            isAudioDevice(device)
        }
        
        if (audioDevices.isNotEmpty()) {
            val device = audioDevices.first()
            viewModel.setUSBDevice(device)
            // Don't auto-connect - wait for user action or USB intent
        } else {
            viewModel.setUSBDevice(null)
        }
    }
    
    private fun refreshUSBDevices() {
        // Refresh and connect to USB device (called by refresh button)
        val deviceList = usbManager.deviceList
        val audioDevices = deviceList.values.filter { device ->
            isAudioDevice(device)
        }
        
        if (audioDevices.isNotEmpty()) {
            val device = audioDevices.first()
            
            // Check if this is SPCMic - use dedicated claiming
            if (device.vendorId == 22564 && device.productId == 10208) {
                claimUsbDevice(device)
            } else {
                viewModel.setUSBDevice(device)
                audioRecorder.connectToDevice(device)
            }
        } else {
            viewModel.setUSBDevice(null)
        }
    }
    
    private fun isAudioDevice(device: UsbDevice): Boolean {
        // Check if device is an audio interface
        // USB Audio Class: Class = 1 (Audio), Subclass = 1 (Audio Control) or 2 (Audio Streaming)
        for (i in 0 until device.interfaceCount) {
            val usbInterface = device.getInterface(i)
            if (usbInterface.interfaceClass == 1) { // USB_CLASS_AUDIO
                return true
            }
        }
        return false
    }
    
    private fun startRecording() {
        if (audioRecorder.startRecording()) {
            viewModel.startRecording()
        } else {
            Toast.makeText(this, "Failed to start recording", Toast.LENGTH_SHORT).show()
        }
    }
    
    private fun stopRecording() {
        audioRecorder.stopRecording()
        viewModel.stopRecording()
    }
    
    private fun formatTime(seconds: Long): String {
        val hours = seconds / 3600
        val minutes = (seconds % 3600) / 60
        val secs = seconds % 60
        return String.format("%02d:%02d:%02d", hours, minutes, secs)
    }
    
    override fun onDestroy() {
        super.onDestroy()
        if (::audioRecorder.isInitialized) {
            audioRecorder.release()
        }
    }
}
package com.spcmic.recorder

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.animation.AnimationUtils
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import androidx.fragment.app.Fragment
import androidx.lifecycle.ViewModelProvider
import com.spcmic.recorder.databinding.FragmentRecordBinding
import java.util.Locale

class RecordFragment : Fragment() {
    
    private var _binding: FragmentRecordBinding? = null
    private val binding get() = _binding!!
    
    private lateinit var viewModel: MainViewModel
    private lateinit var usbManager: UsbManager
    private lateinit var audioRecorder: USBAudioRecorder
    private lateinit var sampleRateAdapter: ArrayAdapter<String>
    private var suppressSampleRateCallback = false
    private var currentSupportedSampleRates: List<Int> = emptyList()
    private var lastSuccessfulSampleRate = 48000
    private var recordingPulseAnimation: android.view.animation.Animation? = null
    private var clipWarningAnimation: android.view.animation.Animation? = null
    private var currentStorageInfo: StorageLocationManager.StorageInfo? = null
    
    private lateinit var requestPermissionLauncher: ActivityResultLauncher<Array<String>>
    private lateinit var storagePickerLauncher: ActivityResultLauncher<Uri?>
    private lateinit var manageStorageLauncher: ActivityResultLauncher<Intent>

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        requestPermissionLauncher = registerForActivityResult(
            ActivityResultContracts.RequestMultiplePermissions()
        ) { permissions ->
            val allGranted = permissions.all { it.value }
            if (allGranted) {
                checkStoragePermissions()
            } else {
                Toast.makeText(requireContext(), "Audio recording permission is required", Toast.LENGTH_LONG).show()
            }
        }
        
        manageStorageLauncher = registerForActivityResult(
            ActivityResultContracts.StartActivityForResult()
        ) { _ ->
            // Check if permission was granted
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                if (Environment.isExternalStorageManager()) {
                    Log.i("RecordFragment", "MANAGE_EXTERNAL_STORAGE permission granted")
                    initializeAudioRecorder()
                } else {
                    Log.w("RecordFragment", "MANAGE_EXTERNAL_STORAGE permission denied - file listing may not work on internal storage")
                    Toast.makeText(requireContext(), 
                        "Storage permission required for accessing recordings on internal storage. You can still use external USB/SD storage.", 
                        Toast.LENGTH_LONG).show()
                    initializeAudioRecorder()
                }
            }
        }

        storagePickerLauncher = registerForActivityResult(
            ActivityResultContracts.OpenDocumentTree()
        ) { uri ->
            if (uri != null) {
                handleStorageSelection(uri)
            }
        }
    }
    
    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentRecordBinding.inflate(inflater, container, false)
        return binding.root
    }
    
    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        
        viewModel = ViewModelProvider(requireActivity())[MainViewModel::class.java]
        usbManager = requireContext().getSystemService(Context.USB_SERVICE) as UsbManager
        lastSuccessfulSampleRate = viewModel.selectedSampleRate.value ?: 48000
        
        setupUI()
        initializeStorageLocation()
        checkPermissions()
        checkBatteryOptimization()
        observeViewModel()
    }
    
    fun handleUsbIntent(intent: Intent) {
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED == intent.action) {
            val usbDevice: UsbDevice? = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            usbDevice?.let { device ->
                Log.i("RecordFragment", "USB device attached: ${device.deviceName} (${device.manufacturerName} ${device.productName})")
                
                // Check if this is our target spcmic device
                if (device.vendorId == 22564 && device.productId == 10208) {
                    Log.i("RecordFragment", "spcmic device detected - claiming device immediately")
                    claimUsbDevice(device)
                } else {
                    Log.i("RecordFragment", "Generic USB audio device detected")
                    refreshUSBDevices()
                }
            }
        }
    }
    
    private fun claimUsbDevice(device: UsbDevice) {
        try {
            // Initialize audio recorder if not already done
            if (!::audioRecorder.isInitialized) {
                audioRecorder = USBAudioRecorder(requireContext(), viewModel)
            }
            
            // Connect to the device immediately
            val success = audioRecorder.connectToDevice(device)
            if (success) {
                viewModel.setUSBDevice(device)
                Toast.makeText(requireContext(), "spcmic claimed successfully!", Toast.LENGTH_SHORT).show()
                Log.i("RecordFragment", "Successfully claimed spcmic device")
            } else {
                Log.e("RecordFragment", "Failed to claim spcmic device")
                Toast.makeText(requireContext(), "Failed to claim spcmic device", Toast.LENGTH_SHORT).show()
            }
        } catch (e: Exception) {
            Log.e("RecordFragment", "Exception while claiming USB device", e)
            Toast.makeText(requireContext(), "Error claiming USB device: ${e.message}", Toast.LENGTH_LONG).show()
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

            sampleRateAdapter = ArrayAdapter(
                requireContext(),
                android.R.layout.simple_spinner_item,
                mutableListOf("48 kHz") // Default placeholder
            )
            sampleRateAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            spinnerSampleRate.adapter = sampleRateAdapter
            spinnerSampleRate.isEnabled = false

            spinnerSampleRate.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                    if (suppressSampleRateCallback) {
                        return
                    }

                    if (position < 0 || position >= currentSupportedSampleRates.size) {
                        return
                    }

                    val selectedRate = currentSupportedSampleRates[position]
                    val currentSelected = viewModel.selectedSampleRate.value
                    if (currentSelected == selectedRate) {
                        return
                    }

                    val recorderReady = ::audioRecorder.isInitialized && viewModel.isUSBDeviceConnected.value == true
                    if (recorderReady) {
                        val applied = audioRecorder.onSampleRateSelected(selectedRate)
                        if (applied) {
                            lastSuccessfulSampleRate = selectedRate
                        } else {
                            Toast.makeText(requireContext(), "Sample rate not accepted by device", Toast.LENGTH_SHORT).show()
                            revertSampleRateSelection()
                        }
                    } else {
                        viewModel.setSelectedSampleRate(selectedRate)
                        lastSuccessfulSampleRate = selectedRate
                    }
                }

                override fun onNothingSelected(parent: AdapterView<*>?) {
                    // No-op
                }
            }

            // Clip indicator - make the whole card clickable to reset
            clipIndicatorContainer.setOnClickListener {
                if (::audioRecorder.isInitialized) {
                    audioRecorder.resetClipIndicator()
                } else {
                    viewModel.clearClipping()
                }
            }

            updateClipIndicator(viewModel.isClipping.value ?: false)
            
            // Settings button opens bottom sheet
            btnSettings.setOnClickListener {
                showSettingsBottomSheet()
            }
        }
    }

    private fun initializeStorageLocation() {
        val info = StorageLocationManager.getStorageInfo(requireContext())
        currentStorageInfo = info
        viewModel.setStoragePath(info.displayPath)
        binding.tvStoragePath.text = info.displayPath
    }
    
    private fun observeViewModel() {
        viewModel.isRecording.observe(viewLifecycleOwner) { isRecording ->
            binding.btnRecord.text = if (isRecording) getString(R.string.record_button_stop) else getString(R.string.record_button_start)
            binding.btnRecord.isEnabled = viewModel.isUSBDeviceConnected.value == true
            
            // Update timer color and card appearance
            val colorRes = if (isRecording) R.color.timecode_recording else R.color.brand_on_surface
            binding.tvRecordingTime.setTextColor(ContextCompat.getColor(requireContext(), colorRes))
            
            // Update button icon
            val iconRes = if (isRecording) R.drawable.ic_stop else R.drawable.ic_microphone
            binding.btnRecord.setIconResource(iconRes)
            
            // Apply recording animations
            if (isRecording) {
                startRecordingAnimations()
            } else {
                stopRecordingAnimations()
            }
        }
        
        viewModel.isUSBDeviceConnected.observe(viewLifecycleOwner) { isConnected ->
            binding.tvConnectionStatus.text = if (isConnected) {
                getString(R.string.usb_device_connected)
            } else {
                getString(R.string.usb_device_not_found)
            }
            binding.btnRecord.isEnabled = isConnected && !viewModel.isRecording.value!!
            binding.spinnerSampleRate.isEnabled = isConnected && currentSupportedSampleRates.isNotEmpty()
            
            // Update connection status badge
            val badgeRes = if (isConnected) R.drawable.status_badge_connected else R.drawable.status_badge_disconnected
            binding.connectionStatusBadge.setBackgroundResource(badgeRes)
        }
        
        viewModel.recordingTime.observe(viewLifecycleOwner) { time ->
            binding.tvRecordingTime.text = formatTime(time)
        }
        
        viewModel.recordingFileName.observe(viewLifecycleOwner) { fileName ->
            binding.tvRecordingFilename.text = if (fileName.isNullOrBlank()) " " else fileName
        }

        viewModel.channelLevels.observe(viewLifecycleOwner) { levels ->
            // Level meter view removed in dashboard design
            // Could be added back as optional overlay if needed
        }

        viewModel.isClipping.observe(viewLifecycleOwner) { isClipping ->
            updateClipIndicator(isClipping)
            
            // Start pulsing animation for clip warning
            if (isClipping) {
                startClipWarningAnimation()
            } else {
                stopClipWarningAnimation()
            }
        }

        viewModel.supportedSampleRates.observe(viewLifecycleOwner) { rates ->
            currentSupportedSampleRates = rates
            if (::sampleRateAdapter.isInitialized) {
                val formatted = rates.map { formatSampleRate(it) }
                sampleRateAdapter.clear()
                if (formatted.isNotEmpty()) {
                    sampleRateAdapter.addAll(formatted)
                }
                sampleRateAdapter.notifyDataSetChanged()
            }
            binding.spinnerSampleRate.isEnabled = viewModel.isUSBDeviceConnected.value == true && rates.isNotEmpty()
            updateSampleRateSpinnerSelection()
            updateSampleRateSupportText()
        }

        viewModel.selectedSampleRate.observe(viewLifecycleOwner) { rate ->
            binding.tvSampleRateStatus.text = "Selected sample rate: ${formatSampleRate(rate)}"
            binding.tvSampleRateInfo.text = "• Sample Rate: ${formatSampleRate(rate)}"
            updateSampleRateSpinnerSelection()
        }

        viewModel.negotiatedSampleRate.observe(viewLifecycleOwner) { rate ->
            binding.tvNegotiatedSampleRate.text = "Device reported rate: ${formatSampleRate(rate)}"
            if (rate > 0) {
                lastSuccessfulSampleRate = rate
            }
        }

        viewModel.supportsContinuousSampleRate.observe(viewLifecycleOwner) {
            updateSampleRateSupportText()
        }

        viewModel.continuousSampleRateRange.observe(viewLifecycleOwner) {
            updateSampleRateSupportText()
        }

        viewModel.storagePath.observe(viewLifecycleOwner) { path ->
            binding.tvStoragePath.text = path
        }
    }

    private fun handleStorageSelection(uri: Uri) {
        try {
            requireContext().contentResolver.takePersistableUriPermission(uri, StorageLocationManager.PERMISSION_FLAGS)
        } catch (_: SecurityException) {
            // Permission may already be granted
        }

        val updated = StorageLocationManager.updateStorageInfo(requireContext(), uri)
        if (updated != null) {
            currentStorageInfo = updated
            viewModel.setStoragePath(updated.displayPath)
            Toast.makeText(requireContext(), getString(R.string.storage_location_updated), Toast.LENGTH_SHORT).show()
        } else {
            Toast.makeText(requireContext(), getString(R.string.storage_location_failed), Toast.LENGTH_SHORT).show()
        }
    }
    
    private fun checkPermissions() {
        val permissionsNeeded = mutableListOf<String>()
        
        if (ContextCompat.checkSelfPermission(requireContext(), Manifest.permission.RECORD_AUDIO) 
            != PackageManager.PERMISSION_GRANTED) {
            permissionsNeeded.add(Manifest.permission.RECORD_AUDIO)
        }
        
        if (permissionsNeeded.isNotEmpty()) {
            requestPermissionLauncher.launch(permissionsNeeded.toTypedArray())
        } else {
            checkStoragePermissions()
        }
    }
    
    private fun checkStoragePermissions() {
        // Android 11+ requires MANAGE_EXTERNAL_STORAGE for broad file access
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                Log.i("RecordFragment", "MANAGE_EXTERNAL_STORAGE not granted - requesting for internal storage access")
                
                AlertDialog.Builder(requireContext())
                    .setTitle("Storage Access Required")
                    .setMessage("To list and playback recordings from internal storage, this app needs file management access.\n\n" +
                               "This is required by Android 11+ for apps that work with audio files.\n\n" +
                               "You can skip this if you only use external USB/SD storage.")
                    .setPositiveButton("Grant Access") { _, _ ->
                        try {
                            val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
                                data = Uri.parse("package:${requireContext().packageName}")
                            }
                            manageStorageLauncher.launch(intent)
                        } catch (e: Exception) {
                            Log.e("RecordFragment", "Failed to request MANAGE_EXTERNAL_STORAGE", e)
                            Toast.makeText(requireContext(), 
                                "Unable to request storage permission. Please enable it manually in Settings.", 
                                Toast.LENGTH_LONG).show()
                            initializeAudioRecorder()
                        }
                    }
                    .setNegativeButton("Skip (Use External Storage)") { _, _ ->
                        Log.i("RecordFragment", "User skipped MANAGE_EXTERNAL_STORAGE - external storage only")
                        initializeAudioRecorder()
                    }
                    .setCancelable(false)
                    .show()
            } else {
                Log.i("RecordFragment", "MANAGE_EXTERNAL_STORAGE already granted")
                initializeAudioRecorder()
            }
        } else {
            // Android 10 and below - no special permission needed
            initializeAudioRecorder()
        }
    }
    
    private fun checkBatteryOptimization() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            val powerManager = requireContext().getSystemService(Context.POWER_SERVICE) as android.os.PowerManager
            val packageName = requireContext().packageName
            
            if (!powerManager.isIgnoringBatteryOptimizations(packageName)) {
                Log.i("RecordFragment", "Battery optimization enabled - requesting exemption for better audio performance")
                
                // Show an informative dialog before requesting
                AlertDialog.Builder(requireContext())
                    .setTitle("Battery Optimization")
                    .setMessage("For best audio recording performance, please disable battery optimization for this app. This ensures uninterrupted recording of 84-channel audio.")
                    .setPositiveButton("Allow") { _, _ ->
                        try {
                            val intent = Intent(android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
                                data = android.net.Uri.parse("package:$packageName")
                            }
                            startActivity(intent)
                        } catch (e: Exception) {
                            Log.e("RecordFragment", "Failed to request battery optimization exemption", e)
                        }
                    }
                    .setNegativeButton("Skip", null)
                    .show()
            } else {
                Log.i("RecordFragment", "Battery optimization already disabled - good for audio performance")
            }
        }
    }
    
    private fun initializeAudioRecorder() {
        audioRecorder = USBAudioRecorder(requireContext(), viewModel)
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
            
            // Check if this is spcmic - use dedicated claiming
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
            requireActivity().window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            
            // Start foreground service for high-priority recording
            AudioRecordingService.startRecordingService(requireContext())
            Log.i("RecordFragment", "Started foreground service for high-priority recording")
        } else {
            Toast.makeText(requireContext(), "Failed to start recording", Toast.LENGTH_SHORT).show()
        }
    }
    
    private fun stopRecording() {
        audioRecorder.stopRecording()
        viewModel.stopRecording()
        requireActivity().window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        
        // Stop foreground service
        AudioRecordingService.stopRecordingService(requireContext())
        Log.i("RecordFragment", "Stopped foreground service")
    }

    private fun updateSampleRateSpinnerSelection() {
        if (!::sampleRateAdapter.isInitialized) {
            return
        }

        val selectedRate = viewModel.selectedSampleRate.value ?: return
        val index = currentSupportedSampleRates.indexOf(selectedRate)

        if (index >= 0 && binding.spinnerSampleRate.selectedItemPosition != index) {
            suppressSampleRateCallback = true
            binding.spinnerSampleRate.setSelection(index, false)
            binding.spinnerSampleRate.post { suppressSampleRateCallback = false }
        }
    }

    private fun revertSampleRateSelection() {
        viewModel.setSelectedSampleRate(lastSuccessfulSampleRate)
        updateSampleRateSpinnerSelection()
    }

    private fun updateSampleRateSupportText() {
        val supportsContinuous = viewModel.supportsContinuousSampleRate.value == true
        val text = if (supportsContinuous) {
            val range = viewModel.continuousSampleRateRange.value
            if (range != null && range.first > 0 && range.second > 0) {
                "Device supports continuous range ${formatSampleRate(range.first)} – ${formatSampleRate(range.second)}"
            } else {
                "Device supports continuous sample rate selection"
            }
        } else {
            if (currentSupportedSampleRates.isEmpty()) {
                "Device sample rate capabilities unknown"
            } else {
                "Device supports discrete sample rates"
            }
        }
        binding.tvSampleRateSupport.text = text
    }

    private fun updateClipIndicator(isClipping: Boolean) {
        val indicator = binding.tvClipIndicator
        val icon = binding.ivClipIcon
        val colorRes = if (isClipping) R.color.clip_indicator_alert else R.color.clip_indicator_idle
        val iconRes = if (isClipping) R.drawable.ic_warning else R.drawable.ic_check_circle
        
        icon.setImageResource(iconRes)
        icon.imageTintList = ColorStateList.valueOf(ContextCompat.getColor(requireContext(), colorRes))
        indicator.text = if (isClipping) "Clip!" else getString(R.string.no_clip_short)
    }

    private fun showSettingsBottomSheet() {
        // Create a bottom sheet dialog for settings
        val bottomSheet = com.google.android.material.bottomsheet.BottomSheetDialog(requireContext())
        val sheetView = layoutInflater.inflate(R.layout.bottom_sheet_settings, null)
        
        // Setup storage location button
        val btnChangeStorage = sheetView.findViewById<com.google.android.material.button.MaterialButton>(R.id.btnChangeStorage)
        val tvStoragePath = sheetView.findViewById<android.widget.TextView>(R.id.tvStoragePathSheet)
        
        tvStoragePath.text = currentStorageInfo?.displayPath ?: "Unknown"
        
        btnChangeStorage.setOnClickListener {
            val initial = currentStorageInfo?.treeUri
            storagePickerLauncher.launch(initial)
            bottomSheet.dismiss()
        }
        
        // Populate device information
        val tvSampleRateStatus = sheetView.findViewById<android.widget.TextView>(R.id.tvSampleRateStatusSheet)
        val tvNegotiatedSampleRate = sheetView.findViewById<android.widget.TextView>(R.id.tvNegotiatedSampleRateSheet)
        val tvSampleRateSupport = sheetView.findViewById<android.widget.TextView>(R.id.tvSampleRateSupportSheet)
        
        tvSampleRateStatus?.text = "Selected: ${formatSampleRate(viewModel.selectedSampleRate.value)}"
        tvNegotiatedSampleRate?.text = "Device: ${formatSampleRate(viewModel.negotiatedSampleRate.value)}"
        
        val supportsContinuous = viewModel.supportsContinuousSampleRate.value == true
        val supportText = if (supportsContinuous) {
            val range = viewModel.continuousSampleRateRange.value
            if (range != null && range.first > 0 && range.second > 0) {
                "Capabilities: ${formatSampleRate(range.first)} – ${formatSampleRate(range.second)}"
            } else {
                "Capabilities: Continuous rate supported"
            }
        } else {
            if (currentSupportedSampleRates.isEmpty()) {
                "Capabilities: Unknown"
            } else {
                "Capabilities: Discrete rates supported"
            }
        }
        tvSampleRateSupport?.text = supportText
        
        // Populate recording configuration
        val tvSampleRateInfo = sheetView.findViewById<android.widget.TextView>(R.id.tvSampleRateInfoSheet)
        tvSampleRateInfo?.text = "• Sample Rate: ${formatSampleRate(viewModel.selectedSampleRate.value)}"
        
        // Add reset clip button
        val btnResetClip = sheetView.findViewById<com.google.android.material.button.MaterialButton>(R.id.btnResetClipSheet)
        btnResetClip?.setOnClickListener {
            if (::audioRecorder.isInitialized) {
                audioRecorder.resetClipIndicator()
            } else {
                viewModel.clearClipping()
            }
            bottomSheet.dismiss()
        }
        
        bottomSheet.setContentView(sheetView)
        bottomSheet.show()
    }

    private fun formatSampleRate(rate: Int?): String {
        val value = rate ?: return "Unknown"
        if (value <= 0) return "Unknown"
        
        // Convert to kHz for cleaner display
        val kHz = value / 1000.0
        
        // Format with appropriate decimal places
        return if (kHz == kHz.toInt().toDouble()) {
            // Whole number (e.g., 48.0 -> "48 kHz")
            String.format(Locale.getDefault(), "%d kHz", kHz.toInt())
        } else {
            // Has decimals (e.g., 44.1 -> "44.1 kHz")
            String.format(Locale.getDefault(), "%.1f kHz", kHz)
        }
    }
    
    private fun formatTime(seconds: Long): String {
        val hours = seconds / 3600
        val minutes = (seconds % 3600) / 60
        val secs = seconds % 60
        return String.format("%02d:%02d:%02d", hours, minutes, secs)
    }
    
    private fun startRecordingAnimations() {
        // Pulse the record button
        if (recordingPulseAnimation == null) {
            recordingPulseAnimation = AnimationUtils.loadAnimation(requireContext(), R.anim.pulse_recording)
        }
        binding.btnRecord.startAnimation(recordingPulseAnimation)
        
        // Update timecode card to recording style
        val timecodeCard = binding.timecodeCard
        timecodeCard.setCardBackgroundColor(ContextCompat.getColor(requireContext(), R.color.card_surface))
        timecodeCard.cardElevation = resources.getDimension(R.dimen.card_elevation_recording)
        timecodeCard.strokeWidth = 0
    }
    
    private fun stopRecordingAnimations() {
        // Stop button pulse
        binding.btnRecord.clearAnimation()
        
        // Reset timecode card to idle style
        val timecodeCard = binding.timecodeCard
        timecodeCard.setCardBackgroundColor(ContextCompat.getColor(requireContext(), R.color.card_surface))
        timecodeCard.cardElevation = resources.getDimension(R.dimen.card_elevation)
        timecodeCard.strokeWidth = resources.getDimensionPixelSize(R.dimen.spacing_xs) / 4
    }
    
    private fun startClipWarningAnimation() {
        if (clipWarningAnimation == null) {
            clipWarningAnimation = AnimationUtils.loadAnimation(requireContext(), R.anim.pulse_clip_warning)
        }
        binding.ivClipIcon.startAnimation(clipWarningAnimation)
    }
    
    private fun stopClipWarningAnimation() {
        binding.ivClipIcon.clearAnimation()
    }
    
    override fun onDestroyView() {
        super.onDestroyView()
        if (::audioRecorder.isInitialized) {
            audioRecorder.release()
        }
        _binding = null
    }
}

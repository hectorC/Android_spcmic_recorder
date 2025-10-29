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
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.fragment.app.Fragment
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import com.google.android.material.materialswitch.MaterialSwitch
import com.spcmic.recorder.databinding.FragmentRecordBinding
import com.spcmic.recorder.location.GpxLocationRepository
import com.spcmic.recorder.location.LocationCaptureManager
import com.spcmic.recorder.location.LocationPreferences
import com.spcmic.recorder.location.LocationStatus
import com.spcmic.recorder.location.RecordingLocation
import com.spcmic.recorder.playback.PlaybackPreferences
import kotlinx.coroutines.Job
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.Locale
import java.util.Date

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
    private var levelMeterJob: Job? = null
    private lateinit var gpxLocationRepository: GpxLocationRepository
    private lateinit var locationCaptureManager: LocationCaptureManager
    private lateinit var locationPermissionLauncher: ActivityResultLauncher<String>
    private var locationSwitchRef: MaterialSwitch? = null
    private var suppressLocationSwitchCallback = false
    
    private lateinit var requestPermissionLauncher: ActivityResultLauncher<Array<String>>
    private lateinit var storagePickerLauncher: ActivityResultLauncher<Uri?>
    private lateinit var manageStorageLauncher: ActivityResultLauncher<Intent>

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        locationPermissionLauncher = registerForActivityResult(
            ActivityResultContracts.RequestPermission()
        ) { granted ->
            handleLocationPermissionResult(granted)
        }

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
    initializeLocationComponents()
        observeViewModel()
        
        // Restore USB connection if fragment is being recreated but audioRecorder exists
        if (::audioRecorder.isInitialized && viewModel.isUSBDeviceConnected.value == true) {
            // Re-verify the connection is still valid after view recreation
            Log.i("RecordFragment", "Restoring USB connection after view recreation")
        }
    }
    
    fun handleUsbIntent(intent: Intent) {
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED == intent.action) {
            val usbDevice: UsbDevice? = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            usbDevice?.let { device ->
                Log.i("RecordFragment", "USB device attached: ${device.deviceName} (${device.manufacturerName} ${device.productName})")
                
                // Check if this is our target spcmic device
                if (device.vendorId == 22564 && device.productId == 10208) {
                    Log.i("RecordFragment", "spcmic device detected - deferring connection to allow USB subsystem to stabilize")
                    // Defer device claiming to allow USB subsystem to fully enumerate the device
                    // This prevents crackle/corruption that can occur when connecting too early during auto-launch
                    binding.root.postDelayed({
                        if (isAdded && viewModel.recorderState.value == RecorderState.IDLE) {
                            Log.i("RecordFragment", "USB subsystem stabilized - claiming spcmic device now")
                            claimUsbDevice(device)
                        } else {
                            Log.w("RecordFragment", "Skipping deferred USB claim - fragment detached or not idle")
                        }
                    }, 500) // 500ms delay to let USB subsystem settle
                } else {
                    Log.i("RecordFragment", "Generic USB audio device detected")
                    refreshUSBDevices()
                }
            }
        }
    }
    
    private fun claimUsbDevice(device: UsbDevice) {
        val state = viewModel.recorderState.value
        if (state != null && state != RecorderState.IDLE) {
            Toast.makeText(requireContext(), "Stop monitoring/recording before refreshing USB", Toast.LENGTH_SHORT).show()
            Log.w("RecordFragment", "Ignoring USB claim while state=$state")
            return
        }

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
                handleRecordButtonClick()
            }
            
            // Long-press to exit monitoring mode without recording
            btnRecord.setOnLongClickListener {
                when (viewModel.recorderState.value) {
                    RecorderState.MONITORING -> {
                        handleExitMonitoring()
                        true // Consume the long click
                    }
                    else -> false // Don't consume - no long-press action for other states
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

                    val recorderState = viewModel.recorderState.value
                    if (recorderState != null && recorderState != RecorderState.IDLE) {
                        Toast.makeText(requireContext(), "Stop monitoring or recording before changing sample rate", Toast.LENGTH_SHORT).show()
                        revertSampleRateSelection()
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
            
            // Gain control slider
            sliderGain.addOnChangeListener { _, value, fromUser ->
                if (fromUser) {
                    viewModel.setGainDb(value)
                    updateGainDisplay(value)
                    
                    // Apply gain to native recorder if initialized
                    if (::audioRecorder.isInitialized) {
                        audioRecorder.setGain(value)
                    }
                }
            }
            
            // Initialize gain display
            val initialGain = viewModel.gainDb.value ?: 0f
            sliderGain.value = initialGain
            updateGainDisplay(initialGain)
            
            // Settings button opens bottom sheet
            btnSettings.setOnClickListener {
                showSettingsBottomSheet()
            }

            val initialStatus = viewModel.locationStatus.value ?: if (viewModel.locationCaptureEnabled.value == true) {
                LocationStatus.Idle
            } else {
                LocationStatus.Disabled
            }
            updateLocationCard(initialStatus, viewModel.locationFix.value)
        }
    }

    private fun updateGainDisplay(gainDb: Float) {
        binding.tvGainValue.text = if (gainDb == 0f) {
            "0 dB"
        } else {
            String.format(Locale.getDefault(), "+%.0f dB", gainDb)
        }
    }

    private fun updateLevelMeter(level: Float) {
        // level is 0.0 to 1.0 (linear amplitude), convert to dBFS for display
        // dBFS = 20 * log10(level), with 0.0 mapped to -∞ dB and 1.0 mapped to 0 dB
        val dbfs = if (level > 0.0001f) {
            20.0f * kotlin.math.log10(level)
        } else {
            -60.0f // Floor at -60 dBFS for display purposes
        }
        
        // Update text to show dBFS
        binding.tvLevelDb.text = String.format("%.1f dBFS", dbfs)
        
        // Professional logarithmic meter scale (like Pro Tools, Logic, etc.)
        // Map dBFS range to bar width: -60 dB = 0%, 0 dB = 100%
        // This gives the characteristic "sensitive at low levels, compressed at high levels" behavior
        val dbMin = -60.0f  // Minimum dB to display
        val dbMax = 0.0f    // Maximum dB (full scale)
        val normalizedDb = (dbfs - dbMin) / (dbMax - dbMin)  // Map -60..0 dB to 0..1
        val barLevel = normalizedDb.coerceIn(0.0f, 1.0f)     // Clamp to valid range
        
        // Update bar width (level grows from left)
        val barContainer = binding.levelMeterBar.parent as? android.widget.FrameLayout
        barContainer?.let { container ->
            val params = binding.levelMeterBar.layoutParams
            params.width = (container.width * barLevel).toInt()
            binding.levelMeterBar.layoutParams = params
        }
        
        // Change color based on dB level (professional thresholds)
        // If clipping, always show red regardless of current level
        val isClipping = viewModel.isClipping.value == true
        val color = when {
            isClipping -> ContextCompat.getColor(requireContext(), R.color.clip_indicator_alert) // Red when clipping
            dbfs < -18.0f -> ContextCompat.getColor(requireContext(), R.color.brand_primary) // Green (safe zone)
            dbfs < -6.0f -> ContextCompat.getColor(requireContext(), android.R.color.holo_orange_light) // Yellow (caution zone)
            else -> ContextCompat.getColor(requireContext(), R.color.clip_indicator_alert) // Red (danger zone, near clipping)
        }
        binding.levelMeterBar.setBackgroundColor(color)
    }

    private fun updateUIForState(state: RecorderState) {
        when (state) {
            RecorderState.IDLE -> {
                binding.btnRecord.text = "START MONITORING"
                binding.btnRecord.setIconResource(R.drawable.ic_levels)
                binding.btnRecord.isEnabled = viewModel.isUSBDeviceConnected.value == true
                // Light purple tint for visibility against dark background
                binding.btnRecord.backgroundTintList = ColorStateList.valueOf(
                    ContextCompat.getColor(requireContext(), R.color.brand_primary_light)
                )
            }
            RecorderState.MONITORING -> {
                binding.btnRecord.text = "START RECORDING"
                binding.btnRecord.setIconResource(R.drawable.ic_microphone)
                binding.btnRecord.isEnabled = true
                // Green tint for "ready to record"
                binding.btnRecord.backgroundTintList = ColorStateList.valueOf(
                    ContextCompat.getColor(requireContext(), R.color.clip_indicator_idle)
                )
            }
            RecorderState.RECORDING -> {
                binding.btnRecord.text = "STOP RECORDING"
                binding.btnRecord.setIconResource(R.drawable.ic_stop)
                binding.btnRecord.isEnabled = true
                // Red tint for "recording active"
                binding.btnRecord.backgroundTintList = ColorStateList.valueOf(
                    ContextCompat.getColor(requireContext(), R.color.clip_indicator_alert)
                )
            }
        }
        updateSampleRateSpinnerEnabled(state)
    }

    private fun updateSampleRateSpinnerEnabled(state: RecorderState? = viewModel.recorderState.value) {
        if (_binding == null) {
            return
        }
        val allowChanges = state == null || state == RecorderState.IDLE
        val isConnected = viewModel.isUSBDeviceConnected.value == true
        val hasRates = currentSupportedSampleRates.isNotEmpty()
        binding.spinnerSampleRate.isEnabled = allowChanges && isConnected && hasRates
    }

    private fun initializeStorageLocation() {
        val info = StorageLocationManager.getStorageInfo(requireContext())
        currentStorageInfo = info
        viewModel.setStoragePath(info.displayPath)
        binding.tvStoragePath.text = info.displayPath
    }

    private fun initializeLocationComponents() {
        gpxLocationRepository = GpxLocationRepository(requireContext())
        locationCaptureManager = LocationCaptureManager(requireContext(), viewModel)

        val captureEnabled = LocationPreferences.isCaptureEnabled(requireContext())
        viewModel.setLocationCaptureEnabled(captureEnabled)
        locationCaptureManager.reset()

        val currentStatus = viewModel.locationStatus.value ?: if (captureEnabled && ContextCompat.checkSelfPermission(requireContext(), Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED) {
            LocationStatus.Idle
        } else if (captureEnabled) {
            LocationStatus.PermissionDenied
        } else {
            LocationStatus.Disabled
        }
        updateLocationCard(currentStatus, viewModel.locationFix.value)
    }
    
    private fun observeViewModel() {
        // Observe the new state machine
        viewModel.recorderState.observe(viewLifecycleOwner) { state ->
            updateUIForState(state)
        }
        
        // Keep legacy isRecording observer for backward compatibility with animations
        viewModel.isRecording.observe(viewLifecycleOwner) { isRecording ->
            // Update timer color and card appearance
            val colorRes = if (isRecording) R.color.timecode_recording else R.color.brand_on_surface
            binding.tvRecordingTime.setTextColor(ContextCompat.getColor(requireContext(), colorRes))
            
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
            updateSampleRateSpinnerEnabled()
            
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

        viewModel.isClipping.observe(viewLifecycleOwner) { isClipping ->
            updateClipIndicator(isClipping)
            
            // Show/hide clip warning icon with animation
            if (isClipping) {
                binding.ivClipWarning.visibility = View.VISIBLE
                startClipWarningAnimation()
            } else {
                binding.ivClipWarning.visibility = View.GONE
                stopClipWarningAnimation()
            }
        }

        viewModel.peakLevel.observe(viewLifecycleOwner) { level ->
            updateLevelMeter(level)
        }

        viewModel.gainDb.observe(viewLifecycleOwner) { gainDb ->
            if (binding.sliderGain.value != gainDb) {
                binding.sliderGain.value = gainDb
                updateGainDisplay(gainDb)
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
            updateSampleRateSpinnerSelection()
            updateSampleRateSupportText()
            updateSampleRateSpinnerEnabled()
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

        viewModel.locationStatus.observe(viewLifecycleOwner) { status ->
            val fix = viewModel.locationFix.value
            updateLocationCard(status, fix)
        }

        viewModel.locationFix.observe(viewLifecycleOwner) { fix ->
            val currentStatus = viewModel.locationStatus.value ?: if (viewModel.locationCaptureEnabled.value == true) {
                LocationStatus.Idle
            } else {
                LocationStatus.Disabled
            }
            updateLocationCard(currentStatus, fix)
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
        val state = viewModel.recorderState.value
        if (state != null && state != RecorderState.IDLE) {
            Toast.makeText(requireContext(), "Stop monitoring/recording before refreshing USB", Toast.LENGTH_SHORT).show()
            Log.w("RecordFragment", "Skipping USB refresh while state=$state")
            return
        }

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
    
    private fun handleRecordButtonClick() {
        when (viewModel.recorderState.value) {
            RecorderState.IDLE -> {
                // Start monitoring (USB streaming, show levels, no file writing)
                val gainDb = viewModel.gainDb.value ?: 0f
                if (audioRecorder.startMonitoring(gainDb)) {
                    viewModel.startMonitoring()
                    Log.i("RecordFragment", "Monitoring started")
                    
                    // Start level meter polling
                    startLevelMeterPolling()
                } else {
                    Toast.makeText(requireContext(), "Failed to start monitoring", Toast.LENGTH_SHORT).show()
                }
            }
            RecorderState.MONITORING -> {
                // Start recording (add file writing to existing USB stream)
                startRecording()
            }
            RecorderState.RECORDING -> {
                // Stop everything (return to IDLE)
                stopRecording()
            }
            else -> {}
        }
    }
    
    private fun startRecording() {
        if (audioRecorder.startRecording()) {
            viewModel.startRecording()
            requireActivity().window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            
            // Start foreground service for high-priority recording
            AudioRecordingService.startRecordingService(requireContext())
            Log.i("RecordFragment", "Started foreground service for high-priority recording")
            
            // Start level meter polling
            startLevelMeterPolling()
        } else {
            Toast.makeText(requireContext(), "Failed to start recording", Toast.LENGTH_SHORT).show()
        }
    }
    
    private fun stopRecording() {
        val latestFileName = viewModel.recordingFileName.value
        val shouldCaptureLocation = viewModel.locationCaptureEnabled.value == true && ::locationCaptureManager.isInitialized

        audioRecorder.stopRecording()
        viewModel.stopRecording()
        requireActivity().window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        
        // Stop foreground service
        AudioRecordingService.stopRecordingService(requireContext())
        Log.i("RecordFragment", "Stopped foreground service")
        
        // Stop level meter polling
        stopLevelMeterPolling()

        if (shouldCaptureLocation && latestFileName != null) {
            viewLifecycleOwner.lifecycleScope.launch {
                val location = locationCaptureManager.captureLocationForRecording()
                if (location != null) {
                    persistRecordingLocation(latestFileName, location)
                }
            }
        }
    }
    
    private fun handleExitMonitoring() {
        // User long-pressed to exit monitoring without recording
        audioRecorder.stopMonitoring()
        viewModel.stopMonitoring()
        
        // Stop level meter polling
        stopLevelMeterPolling()
        
        Toast.makeText(requireContext(), "Monitoring cancelled", Toast.LENGTH_SHORT).show()
        Log.i("RecordFragment", "Monitoring cancelled via long-press")
    }

    private fun startLevelMeterPolling() {
        // Cancel any existing job
        levelMeterJob?.cancel()
        
        // Start new polling job
        levelMeterJob = viewLifecycleOwner.lifecycleScope.launch {
            while (isActive && ::audioRecorder.isInitialized) {
                try {
                    val peakLevel = audioRecorder.getPeakLevel()
                    viewModel.setPeakLevel(peakLevel)

                    // Also check the native clip flag so monitoring mode latches the warning
                    if (audioRecorder.hasClippedNative() && viewModel.isClipping.value != true) {
                        viewModel.setClipping(true)
                    }
                } catch (e: Exception) {
                    Log.e("RecordFragment", "Error polling peak level", e)
                }
                delay(100) // Poll every 100ms for smooth updates
            }
        }
    }

    private fun stopLevelMeterPolling() {
        levelMeterJob?.cancel()
        levelMeterJob = null
        viewModel.setPeakLevel(0f) // Reset level meter to zero
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
        // Clip warning is now integrated into the level meter
        // Just show/hide the warning icon
        binding.ivClipWarning.visibility = if (isClipping) View.VISIBLE else View.GONE
    }

    private fun handleLocationPermissionResult(granted: Boolean) {
        if (!isAdded) {
            return
        }

        val switch = locationSwitchRef
        suppressLocationSwitchCallback = true
        if (granted) {
            enableLocationCapture()
            switch?.isChecked = true
        } else {
            LocationPreferences.setCaptureEnabled(requireContext(), false)
            viewModel.setLocationCaptureEnabled(false)
            viewModel.updateLocationStatus(LocationStatus.PermissionDenied)
            viewModel.updateLocationFix(null)
            switch?.isChecked = false
            Toast.makeText(requireContext(), getString(R.string.location_status_permission_denied), Toast.LENGTH_SHORT).show()
        }
        suppressLocationSwitchCallback = false
    }

    private fun handleLocationSwitchChanged(switch: MaterialSwitch, enabled: Boolean) {
        if (enabled) {
            if (ContextCompat.checkSelfPermission(requireContext(), Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED) {
                enableLocationCapture()
            } else {
                val rationaleNeeded = shouldShowRequestPermissionRationale(Manifest.permission.ACCESS_FINE_LOCATION)
                if (rationaleNeeded) {
                    AlertDialog.Builder(requireContext())
                        .setTitle(R.string.location_settings_title)
                        .setMessage(R.string.location_permission_rationale)
                        .setPositiveButton(android.R.string.ok) { _, _ ->
                            locationPermissionLauncher.launch(Manifest.permission.ACCESS_FINE_LOCATION)
                        }
                        .setNegativeButton(android.R.string.cancel) { _, _ ->
                            setSwitchCheckedWithoutCallback(switch, false)
                        }
                        .setCancelable(false)
                        .show()
                } else {
                    locationPermissionLauncher.launch(Manifest.permission.ACCESS_FINE_LOCATION)
                }
            }
        } else {
            disableLocationCapture()
            setSwitchCheckedWithoutCallback(switch, false)
        }
    }

    private fun setSwitchCheckedWithoutCallback(switch: MaterialSwitch, checked: Boolean) {
        suppressLocationSwitchCallback = true
        switch.isChecked = checked
        suppressLocationSwitchCallback = false
    }

    private fun enableLocationCapture() {
        if (!::locationCaptureManager.isInitialized) return
        LocationPreferences.setCaptureEnabled(requireContext(), true)
        viewModel.setLocationCaptureEnabled(true)
        locationCaptureManager.reset()
    }

    private fun disableLocationCapture() {
        if (!::locationCaptureManager.isInitialized) return
        LocationPreferences.setCaptureEnabled(requireContext(), false)
        viewModel.setLocationCaptureEnabled(false)
        locationCaptureManager.reset()
    }

    private fun updateLocationCard(status: LocationStatus, fix: RecordingLocation?) {
        if (!isAdded || _binding == null) {
            return
        }

        val locationEnabled = viewModel.locationCaptureEnabled.value == true
        val locationIcon = binding.ivTimecodeLocation

        if (!locationEnabled) {
            locationIcon.visibility = View.GONE
            return
        }

        val tintColor = when (status) {
            LocationStatus.PermissionDenied, is LocationStatus.Unavailable -> R.color.clip_indicator_alert
            LocationStatus.Disabled -> R.color.brand_on_surface_variant
            else -> R.color.brand_primary
        }
        locationIcon.setColorFilter(ContextCompat.getColor(requireContext(), tintColor))
        locationIcon.visibility = View.VISIBLE

        locationIcon.contentDescription = when (status) {
            LocationStatus.Disabled -> getString(R.string.location_status_disabled)
            LocationStatus.Idle -> getString(R.string.location_status_idle)
            LocationStatus.Acquiring -> getString(R.string.location_status_acquiring)
            is LocationStatus.Ready -> buildLocationDetails(fix) ?: getString(R.string.location_status_idle)
            is LocationStatus.Unavailable -> status.reason ?: getString(R.string.location_status_unavailable)
            LocationStatus.PermissionDenied -> getString(R.string.location_status_permission_denied)
        }
    }

    private fun buildLocationDetails(fix: RecordingLocation?): String? {
        if (fix == null) return null
        val parts = mutableListOf<String>()
        fix.accuracyMeters?.let {
            parts.add(getString(R.string.location_accuracy_format, it))
        }
        val timestamp = Date(fix.timestampMillisUtc)
        parts.add(getString(R.string.location_timestamp_format, timestamp))
        return parts.joinToString(" • ")
    }

    private fun persistRecordingLocation(fileName: String, location: RecordingLocation) {
        if (!::gpxLocationRepository.isInitialized) return
        val storageInfo = currentStorageInfo ?: StorageLocationManager.getStorageInfo(requireContext())
        viewLifecycleOwner.lifecycleScope.launch(Dispatchers.IO) {
            runCatching {
                gpxLocationRepository.upsertLocation(storageInfo, fileName, location)
            }.onFailure {
                Log.e("RecordFragment", "Failed to persist location for $fileName", it)
            }
        }
    }

    private fun showSettingsBottomSheet() {
        // Create a bottom sheet dialog for settings
        val bottomSheet = com.google.android.material.bottomsheet.BottomSheetDialog(requireContext())
        val sheetView = layoutInflater.inflate(R.layout.bottom_sheet_settings, null)

        val basePaddingLeft = sheetView.paddingLeft
        val basePaddingTop = sheetView.paddingTop
        val basePaddingRight = sheetView.paddingRight
        val basePaddingBottom = sheetView.paddingBottom
        val insetSpacer = sheetView.findViewById<View>(R.id.bottomInsetSpacer)
        ViewCompat.setOnApplyWindowInsetsListener(sheetView) { v, insets ->
            val systemBars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            v.setPadding(basePaddingLeft, basePaddingTop, basePaddingRight, basePaddingBottom)
            insetSpacer?.let { spacer ->
                val params = spacer.layoutParams
                val targetHeight = systemBars.bottom
                if (params.height != targetHeight) {
                    params.height = targetHeight
                    spacer.layoutParams = params
                }
            }
            insets
        }
        ViewCompat.requestApplyInsets(sheetView)
        
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
        
        val switchLocationCapture = sheetView.findViewById<MaterialSwitch>(R.id.switchLocationCapture)
        locationSwitchRef = switchLocationCapture
        suppressLocationSwitchCallback = true
        switchLocationCapture.isChecked = viewModel.locationCaptureEnabled.value == true
        suppressLocationSwitchCallback = false
        switchLocationCapture.setOnCheckedChangeListener { _, isChecked ->
            if (suppressLocationSwitchCallback) {
                return@setOnCheckedChangeListener
            }
            handleLocationSwitchChanged(switchLocationCapture, isChecked)
        }

        val switchRealtimeConvolution = sheetView.findViewById<MaterialSwitch>(R.id.switchRealtimeConvolution)
        switchRealtimeConvolution?.let { realtimeSwitch ->
            realtimeSwitch.isChecked = PlaybackPreferences.isRealtimeConvolutionEnabled(requireContext())
            realtimeSwitch.setOnCheckedChangeListener { _, isChecked ->
                PlaybackPreferences.setRealtimeConvolutionEnabled(requireContext(), isChecked)
            }
        }

        bottomSheet.setContentView(sheetView)
        bottomSheet.setOnDismissListener {
            locationSwitchRef = null
        }
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
        binding.ivClipWarning.startAnimation(clipWarningAnimation)
    }
    
    private fun stopClipWarningAnimation() {
        binding.ivClipWarning.clearAnimation()
    }
    
    override fun onDestroyView() {
        super.onDestroyView()
        // Only clean up view-related resources here
        // USB connection survives because fragment is just hidden, not destroyed
        stopLevelMeterPolling()
        recordingPulseAnimation = null
        clipWarningAnimation = null
        locationSwitchRef = null
        _binding = null
    }
    
    override fun onDestroy() {
        super.onDestroy()
        // Fragment is being truly destroyed (app closing, not just hidden)
        // Clean up all resources including USB connection
        if (::audioRecorder.isInitialized) {
            audioRecorder.release()
        }
    }
}

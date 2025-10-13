package com.spcmic.recorder.playback

import android.app.AlertDialog
import android.content.Context
import android.graphics.Color
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.PopupMenu
import android.widget.SeekBar
import android.widget.Toast
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import androidx.lifecycle.ViewModelProvider
import androidx.recyclerview.widget.LinearLayoutManager
import com.spcmic.recorder.R
import com.spcmic.recorder.StorageLocationManager
import com.spcmic.recorder.databinding.FragmentPlaybackBinding
import java.io.File
import kotlin.math.abs

/**
 * Fragment for playback functionality
 */
class PlaybackFragment : Fragment() {
    
    private var _binding: FragmentPlaybackBinding? = null
    private val binding get() = _binding!!
    
    private lateinit var viewModel: PlaybackViewModel
    private lateinit var adapter: RecordingListAdapter
    
    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentPlaybackBinding.inflate(inflater, container, false)
        return binding.root
    }
    
    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        
        viewModel = ViewModelProvider(this)[PlaybackViewModel::class.java]
    viewModel.attachContext(requireContext().applicationContext)
        viewModel.setAssetManager(requireContext().assets)
        val preferences = requireContext().getSharedPreferences(PlaybackViewModel.PREFS_NAME, Context.MODE_PRIVATE)
        viewModel.setPreferences(preferences)

        val cacheDir = File(requireContext().filesDir, "playback_cache")
        if (!cacheDir.exists()) {
            cacheDir.mkdirs()
        }
        viewModel.setCacheDirectory(cacheDir.absolutePath)

        initializeStorageLocation()
        setupRecyclerView()
        setupPlayerControls()
        observeViewModel()
    }
    
    private fun setupRecyclerView() {
        adapter = RecordingListAdapter(
            onRecordingClick = { recording ->
                viewModel.selectRecording(recording)
            },
            onMoreOptionsClick = { recording, view ->
                showMoreOptionsMenu(recording, view)
            }
        )
        
        binding.recyclerViewRecordings.apply {
            layoutManager = LinearLayoutManager(context)
            adapter = this@PlaybackFragment.adapter
        }
    }
    
    private fun setupPlayerControls() {
        // Play/Pause button
        binding.playerControls.btnPlayPause.setOnClickListener {
            val isPlaying = viewModel.isPlaying.value == true
            if (isPlaying) {
                viewModel.pause()
            } else {
                viewModel.play()
            }
        }
        
        // Stop button
        binding.playerControls.btnStop.setOnClickListener {
            viewModel.stopPlayback()
        }
        
        // Close player
        binding.playerControls.btnClosePlayer.setOnClickListener {
            viewModel.clearSelection()
        }
        
        // Timeline seekbar
        binding.playerControls.seekBarTimeline.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                if (fromUser) {
                    val duration = viewModel.totalDuration.value ?: 0L
                    val positionMs = (progress / 1000f * duration).toLong()
                    viewModel.seekTo(positionMs)
                }
            }
            
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        val initialGain = viewModel.playbackGainDb.value ?: 0f
        binding.playerControls.sliderGain.value = initialGain
        binding.playerControls.tvGainValue.text = formatGain(initialGain)

        binding.playerControls.sliderGain.addOnChangeListener { _, value, fromUser ->
            if (fromUser) {
                viewModel.setPlaybackGain(value)
            }
        }

        updateLoopButton(viewModel.isLooping.value == true)
        binding.playerControls.btnLoop.setOnClickListener {
            val currentlyLooping = viewModel.isLooping.value == true
            viewModel.setLooping(!currentlyLooping)
        }
    }

    private fun observeViewModel() {
        // Observe recordings list
        viewModel.recordings.observe(viewLifecycleOwner) { recordings ->
            adapter.submitList(recordings)
            
            // Update storage info
            val count = recordings.size
            val countText = if (count == 1) "1 recording" else "$count recordings"
            binding.tvRecordingCount.text = countText
            binding.tvStorageUsed.text = viewModel.getFormattedStorageSize()
            
            // Show/hide empty state
            if (recordings.isEmpty()) {
                binding.layoutEmptyState.visibility = View.VISIBLE
                binding.recyclerViewRecordings.visibility = View.GONE
            } else {
                binding.layoutEmptyState.visibility = View.GONE
                binding.recyclerViewRecordings.visibility = View.VISIBLE
            }
        }
        
        // Observe selected recording
        viewModel.selectedRecording.observe(viewLifecycleOwner) { recording ->
            if (recording != null) {
                binding.playerControls.root.visibility = View.VISIBLE
                binding.playerControls.tvNowPlayingFile.text = recording.fileName.removeSuffix(".wav")
                binding.playerControls.tvTotalDuration.text = recording.formattedDuration
            } else {
                binding.playerControls.root.visibility = View.GONE
            }
        }
        
        // Observe playback state
        viewModel.isPlaying.observe(viewLifecycleOwner) { isPlaying ->
            val iconRes = if (isPlaying) R.drawable.ic_pause else R.drawable.ic_play
            binding.playerControls.btnPlayPause.setImageResource(iconRes)
        }

        viewModel.isProcessing.observe(viewLifecycleOwner) { processing ->
            if (processing) {
                binding.preprocessOverlay.bringToFront()
            }
            binding.preprocessOverlay.visibility = if (processing) View.VISIBLE else View.GONE
            binding.playerControls.btnPlayPause.isEnabled = !processing
            binding.playerControls.btnStop.isEnabled = !processing
            binding.playerControls.seekBarTimeline.isEnabled = !processing
            binding.playerControls.sliderGain.isEnabled = !processing
            binding.playerControls.btnLoop.isEnabled = !processing
            if (!processing) {
                binding.progressPreprocess.progress = 0
                binding.tvPreprocessPercent.text = getString(R.string.percent_format, 0)
            }
        }

        viewModel.processingMessage.observe(viewLifecycleOwner) { messageRes ->
            binding.tvPreprocessMessage.setText(messageRes)
        }

        viewModel.processingProgress.observe(viewLifecycleOwner) { percent ->
            binding.progressPreprocess.progress = percent
            binding.tvPreprocessPercent.text = getString(R.string.percent_format, percent)
        }

        viewModel.isLooping.observe(viewLifecycleOwner) { looping ->
            updateLoopButton(looping)
        }

        viewModel.playbackGainDb.observe(viewLifecycleOwner) { gainDb ->
            val slider = binding.playerControls.sliderGain
            if (abs(slider.value - gainDb) > 0.01f) {
                slider.value = gainDb
            }
            binding.playerControls.tvGainValue.text = formatGain(gainDb)
        }
        
        // Observe current position
        viewModel.currentPosition.observe(viewLifecycleOwner) { positionMs ->
            val totalMs = viewModel.totalDuration.value ?: 0L
            if (totalMs > 0) {
                val progress = ((positionMs.toFloat() / totalMs) * 1000).toInt()
                binding.playerControls.seekBarTimeline.progress = progress
            }
            
            // Update time label
            val seconds = (positionMs / 1000).toInt()
            val minutes = seconds / 60
            val secs = seconds % 60
            binding.playerControls.tvCurrentPosition.text = String.format("%d:%02d", minutes, secs)
        }
        viewModel.statusMessage.observe(viewLifecycleOwner) { message ->
            message?.let {
                val text = if (it.args.isEmpty()) {
                    getString(it.resId)
                } else {
                    getString(it.resId, *it.args.toTypedArray())
                }
                Toast.makeText(requireContext(), text, Toast.LENGTH_SHORT).show()
                viewModel.clearStatusMessage()
            }
        }

        viewModel.storagePath.observe(viewLifecycleOwner) { path ->
            binding.tvPlaybackStoragePath.text = path
        }
    }
    
    private fun showMoreOptionsMenu(recording: Recording, view: View) {
        PopupMenu(requireContext(), view).apply {
            menuInflater.inflate(R.menu.recording_options, menu)
            setOnMenuItemClickListener { menuItem ->
                when (menuItem.itemId) {
                    R.id.action_delete -> {
                        confirmDelete(recording)
                        true
                    }
                    R.id.action_export_binaural -> {
                        viewModel.exportRecording(recording)
                        true
                    }
                    else -> false
                }
            }
            show()
        }
    }
    
    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    private fun formatGain(gainDb: Float): String {
        return if (gainDb < 0.05f) {
            getString(R.string.gain_value_zero)
        } else {
            getString(R.string.gain_value_format, gainDb)
        }
    }

    private fun confirmDelete(recording: Recording) {
        AlertDialog.Builder(requireContext())
            .setTitle(R.string.delete)
            .setMessage(R.string.delete_confirmation_message)
            .setPositiveButton(R.string.delete) { _, _ ->
                viewModel.deleteRecording(recording)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun updateLoopButton(looping: Boolean) {
        val tintColor = if (looping) {
            ContextCompat.getColor(requireContext(), R.color.purple_500)
        } else {
            Color.parseColor("#66FFFFFF")
        }
        binding.playerControls.btnLoop.setColorFilter(tintColor)
        binding.playerControls.btnLoop.alpha = if (looping) 1f else 0.8f
    }

    private fun initializeStorageLocation() {
        val info = StorageLocationManager.getStorageInfo(requireContext())
        viewModel.updateStorageLocation(info)
        binding.tvPlaybackStoragePath.text = info.displayPath
        viewModel.scanRecordings()
    }

    override fun onResume() {
        super.onResume()
        // Refresh in case the record tab updated the storage preference
        initializeStorageLocation()
    }
}

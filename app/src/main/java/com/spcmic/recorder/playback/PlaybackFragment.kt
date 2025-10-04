package com.spcmic.recorder.playback

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.PopupMenu
import androidx.fragment.app.Fragment
import androidx.lifecycle.ViewModelProvider
import androidx.recyclerview.widget.LinearLayoutManager
import com.spcmic.recorder.R
import com.spcmic.recorder.databinding.FragmentPlaybackBinding

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
        
        setupRecyclerView()
        setupPlayerControls()
        observeViewModel()
        
        // Scan for recordings on start
        viewModel.scanRecordings()
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
        // Player control buttons (non-functional for now)
        binding.playerControls.btnPlayPause.setOnClickListener {
            // TODO: Implement play/pause in Sprint 2
            val isPlaying = viewModel.isPlaying.value == true
            viewModel.setPlaying(!isPlaying)
        }
        
        binding.playerControls.btnStop.setOnClickListener {
            // TODO: Implement stop in Sprint 2
            viewModel.setPlaying(false)
            viewModel.updatePosition(0L)
        }
        
        binding.playerControls.btnClosePlayer.setOnClickListener {
            viewModel.clearSelection()
        }
        
        // Timeline seekbar (non-functional for now)
        binding.playerControls.seekBarTimeline.setOnSeekBarChangeListener(null) // TODO: Add in Sprint 2
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
    }
    
    private fun showMoreOptionsMenu(recording: Recording, view: View) {
        PopupMenu(requireContext(), view).apply {
            menuInflater.inflate(R.menu.recording_options, menu)
            setOnMenuItemClickListener { menuItem ->
                when (menuItem.itemId) {
                    R.id.action_delete -> {
                        // TODO: Implement delete confirmation dialog
                        true
                    }
                    R.id.action_share -> {
                        // TODO: Implement share functionality
                        true
                    }
                    R.id.action_details -> {
                        // TODO: Show file details dialog
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
}

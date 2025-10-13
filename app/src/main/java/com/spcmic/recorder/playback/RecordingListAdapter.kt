package com.spcmic.recorder.playback

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.spcmic.recorder.R
import java.text.SimpleDateFormat
import java.util.*

/**
 * RecyclerView adapter for displaying list of recordings
 */
class RecordingListAdapter(
    private val onRecordingClick: (Recording) -> Unit,
    private val onMoreOptionsClick: (Recording, View) -> Unit
) : ListAdapter<Recording, RecordingListAdapter.RecordingViewHolder>(RecordingDiffCallback()) {
    
    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecordingViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_recording, parent, false)
        return RecordingViewHolder(view, onRecordingClick, onMoreOptionsClick)
    }
    
    override fun onBindViewHolder(holder: RecordingViewHolder, position: Int) {
        holder.bind(getItem(position))
    }
    
    class RecordingViewHolder(
        itemView: View,
        private val onRecordingClick: (Recording) -> Unit,
        private val onMoreOptionsClick: (Recording, View) -> Unit
    ) : RecyclerView.ViewHolder(itemView) {
        
        private val tvFileName: TextView = itemView.findViewById(R.id.tvFileName)
        private val tvDateTime: TextView = itemView.findViewById(R.id.tvDateTime)
        private val tvDuration: TextView = itemView.findViewById(R.id.tvDuration)
        private val tvSampleRate: TextView = itemView.findViewById(R.id.tvSampleRate)
        private val tvFileSize: TextView = itemView.findViewById(R.id.tvFileSize)
        private val ivPlayIcon: ImageView = itemView.findViewById(R.id.ivPlayIcon)
        private val ivMoreOptions: ImageView = itemView.findViewById(R.id.ivMoreOptions)
        
        private val dateFormat = SimpleDateFormat("MMM d, yyyy • h:mm a", Locale.getDefault())
        
        fun bind(recording: Recording) {
            // Set filename (remove extension for cleaner display)
            val displayName = recording.fileName.removeSuffix(".wav")
            tvFileName.text = displayName
            
            // Set date and time
            val date = Date(recording.dateTime)
            tvDateTime.text = dateFormat.format(date)
            
            // Set duration
            tvDuration.text = recording.formattedDuration
            
            // Set sample rate
            tvSampleRate.text = recording.formattedSampleRate
            
            // Set file size
            tvFileSize.text = recording.formattedFileSize
            
            // Set click listeners
            itemView.setOnClickListener {
                onRecordingClick(recording)
            }
            
            ivPlayIcon.setOnClickListener {
                onRecordingClick(recording)
            }
            
            ivMoreOptions.setOnClickListener {
                onMoreOptionsClick(recording, it)
            }
        }
    }
    
    private class RecordingDiffCallback : DiffUtil.ItemCallback<Recording>() {
        override fun areItemsTheSame(oldItem: Recording, newItem: Recording): Boolean {
            return oldItem.uniqueId == newItem.uniqueId
        }
        
        override fun areContentsTheSame(oldItem: Recording, newItem: Recording): Boolean {
            return oldItem == newItem
        }
    }
}

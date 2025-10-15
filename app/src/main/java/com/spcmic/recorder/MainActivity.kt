package com.spcmic.recorder

import android.content.Intent
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import androidx.fragment.app.Fragment
import com.spcmic.recorder.databinding.ActivityMainBinding
import com.spcmic.recorder.playback.PlaybackFragment

class MainActivity : AppCompatActivity() {
    
    private lateinit var binding: ActivityMainBinding
    private var recordFragment: RecordFragment? = null
    private var playbackFragment: PlaybackFragment? = null
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        
        // Setup bottom navigation
        binding.bottomNavigation.setOnItemSelectedListener { item ->
            when (item.itemId) {
                R.id.navigation_record -> {
                    showRecordFragment()
                    true
                }
                R.id.navigation_playback -> {
                    showPlaybackFragment()
                    true
                }
                else -> false
            }
        }
        
        // Show record fragment by default
        if (savedInstanceState == null) {
            showRecordFragment()
        }
        
        // Handle USB device attachment if app was launched by USB intent
        // Defer until fragment is attached
        binding.root.post {
            handleUsbIntent(intent)
        }
    }
    
    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        intent?.let { handleUsbIntent(it) }
    }
    
    private fun handleUsbIntent(intent: Intent) {
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED == intent.action) {
            // Forward USB intent to RecordFragment if it's attached
            recordFragment?.takeIf { it.isAdded }?.handleUsbIntent(intent)
        }
    }
    
    private fun showRecordFragment() {
        if (recordFragment == null) {
            recordFragment = RecordFragment()
            supportFragmentManager.beginTransaction()
                .add(R.id.fragmentContainer, recordFragment!!, "record")
                .commit()
        }
        
        // Show record, hide playback
        supportFragmentManager.beginTransaction().apply {
            recordFragment?.let { show(it) }
            playbackFragment?.let { hide(it) }
            commit()
        }
    }
    
    private fun showPlaybackFragment() {
        if (playbackFragment == null) {
            playbackFragment = PlaybackFragment()
            supportFragmentManager.beginTransaction()
                .add(R.id.fragmentContainer, playbackFragment!!, "playback")
                .commit()
        }
        
        // Show playback, hide record
        supportFragmentManager.beginTransaction().apply {
            playbackFragment?.let { show(it) }
            recordFragment?.let { hide(it) }
            commit()
        }
    }
    
    private fun replaceFragment(fragment: Fragment) {
        // Deprecated - using show/hide pattern instead
        supportFragmentManager.beginTransaction()
            .replace(R.id.fragmentContainer, fragment)
            .commit()
    }
}
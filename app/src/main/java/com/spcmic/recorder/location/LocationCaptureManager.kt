package com.spcmic.recorder.location

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.location.Location
import android.util.Log
import androidx.core.content.ContextCompat
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority
import com.google.android.gms.tasks.CancellationTokenSource
import com.spcmic.recorder.MainViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

/**
 * Handles on-demand fused location lookups and propagates status updates to the shared view model.
 */
class LocationCaptureManager(
    private val context: Context,
    private val viewModel: MainViewModel
) {
    private val fusedClient = LocationServices.getFusedLocationProviderClient(context)
    private var lastFix: RecordingLocation? = null

    fun reset() {
        val captureEnabled = viewModel.locationCaptureEnabled.value == true

        if (!captureEnabled) {
            lastFix = null
            viewModel.updateLocationFix(null)
            viewModel.updateLocationStatus(LocationStatus.Disabled)
            return
        }

        if (!hasPermission()) {
            lastFix = null
            viewModel.updateLocationFix(null)
            viewModel.updateLocationStatus(LocationStatus.PermissionDenied)
            return
        }

        val existingFix = viewModel.locationFix.value ?: lastFix
        if (existingFix != null) {
            lastFix = existingFix
            viewModel.updateLocationFix(existingFix)
            viewModel.updateLocationStatus(LocationStatus.Ready(existingFix.accuracyMeters))
        } else {
            viewModel.updateLocationStatus(LocationStatus.Idle)
        }
    }

    suspend fun captureLocationForRecording(): RecordingLocation? {
        if (viewModel.locationCaptureEnabled.value != true) {
            return null
        }

        if (!hasPermission()) {
            viewModel.updateLocationStatus(LocationStatus.PermissionDenied)
            viewModel.updateLocationFix(null)
            return null
        }

        viewModel.updateLocationStatus(LocationStatus.Acquiring)

        val location = withContext(Dispatchers.IO) {
            getCurrentLocation() ?: getLastKnownLocation()
        }

        val recordingLocation = location?.let { toRecordingLocation(it) }

        if (recordingLocation != null) {
            lastFix = recordingLocation
            viewModel.updateLocationFix(recordingLocation)
            viewModel.updateLocationStatus(LocationStatus.Ready(recordingLocation.accuracyMeters))
        } else {
            viewModel.updateLocationFix(null)
            viewModel.updateLocationStatus(LocationStatus.Unavailable())
        }

        return recordingLocation
    }

    fun lastKnownFix(): RecordingLocation? = lastFix

    private fun hasPermission(): Boolean {
        return ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
    }

    @SuppressLint("MissingPermission")
    private suspend fun getCurrentLocation(): Location? = suspendCancellableCoroutine { cont ->
        val tokenSource = CancellationTokenSource()
        val task = fusedClient.getCurrentLocation(Priority.PRIORITY_HIGH_ACCURACY, tokenSource.token)
        task.addOnSuccessListener { location ->
            cont.resume(location)
        }.addOnFailureListener { throwable ->
            Log.w(TAG, "getCurrentLocation failed", throwable)
            cont.resume(null)
        }.addOnCanceledListener {
            cont.resume(null)
        }

        cont.invokeOnCancellation {
            tokenSource.cancel()
        }
    }

    @SuppressLint("MissingPermission")
    private suspend fun getLastKnownLocation(): Location? = suspendCancellableCoroutine { cont ->
        fusedClient.lastLocation
            .addOnSuccessListener { location -> cont.resume(location) }
            .addOnFailureListener { throwable ->
                Log.w(TAG, "lastLocation retrieval failed", throwable)
                cont.resume(null)
            }
            .addOnCanceledListener { cont.resume(null) }
    }

    private fun toRecordingLocation(location: Location): RecordingLocation {
        val timestamp = if (location.time > 0) location.time else System.currentTimeMillis()
        return RecordingLocation(
            latitude = location.latitude,
            longitude = location.longitude,
            accuracyMeters = location.accuracy.takeIf { it > 0f },
            altitudeMeters = if (location.hasAltitude()) location.altitude else null,
            timestampMillisUtc = timestamp,
            provider = location.provider
        )
    }

    companion object {
        private const val TAG = "LocationCaptureMgr"
    }
}

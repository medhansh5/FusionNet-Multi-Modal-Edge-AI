package com.medhansh.fusionnet

import android.app.*
import android.content.Context
import android.content.Intent
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.camera.core.*
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleService
import java.io.File
import java.io.FileWriter
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.*
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * FusionDataService is a foreground service that captures accelerometer data
 * and records video using CameraX, synchronizing telemetry and video metadata
 */
class FusionDataService : LifecycleService(), SensorEventListener {
    
    companion object {
        private const val TAG = "FusionDataService"
        private const val NOTIFICATION_ID = 1001
        private const val CHANNEL_ID = "FusionDataChannel"
        private const val SAMPLE_RATE_HZ = 100f
        private const val CUTOFF_FREQUENCY_HZ = 30f
        private const val VIDEO_FPS = 30
        
        // CSV file headers
        private const val TELEMETRY_HEADER = "timestamp_ns,filtered_x,filtered_y,filtered_z"
        private const val VIDEO_METADATA_HEADER = "timestamp_ns,frame_index"
    }
    
    // Native filter instance
    private lateinit var nativeFilter: NativeFilter
    
    // Sensor management
    private lateinit var sensorManager: SensorManager
    private var accelerometer: Sensor? = null
    
    // CameraX components
    private lateinit var cameraProvider: ProcessCameraProvider
    private lateinit var cameraExecutor: ExecutorService
    private var videoCapture: VideoCapture<Recorder>? = null
    private var currentRecording: Recording? = null
    private var frameCounter = 0L
    
    // CSV file writers
    private var telemetryWriter: FileWriter? = null
    private var videoMetadataWriter: FileWriter? = null
    
    // Timing and synchronization
    private var lastSensorEventTime = 0L
    private var sensorEventCount = 0L
    private var missedSensorEvents = 0L
    
    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "FusionDataService onCreate")
        
        initializeComponents()
        createNotificationChannel()
        startForeground(NOTIFICATION_ID, createNotification())
    }
    
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        Log.i(TAG, "FusionDataService onStartCommand")
        
        startDataCapture()
        return START_STICKY
    }
    
    override fun onDestroy() {
        super.onDestroy()
        Log.i(TAG, "FusionDataService onDestroy")
        
        stopDataCapture()
        cleanup()
    }
    
    private fun initializeComponents() {
        try {
            // Initialize native filter
            nativeFilter = NativeFilter()
            if (!nativeFilter.initFilter(SAMPLE_RATE_HZ, CUTOFF_FREQUENCY_HZ)) {
                throw RuntimeException("Failed to initialize native filter")
            }
            
            // Initialize sensor manager
            sensorManager = getSystemService(Context.SENSOR_SERVICE) as SensorManager
            accelerometer = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
            
            if (accelerometer == null) {
                throw RuntimeException("Accelerometer not available")
            }
            
            // Initialize camera executor
            cameraExecutor = Executors.newSingleThreadExecutor()
            
            // Initialize CSV files
            initializeCsvFiles()
            
            Log.i(TAG, "All components initialized successfully")
            
        } catch (e: Exception) {
            Log.e(TAG, "Failed to initialize components", e)
            throw e
        }
    }
    
    private fun initializeCsvFiles() {
        try {
            val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(Date())
            val filesDir = getExternalFilesDir(null)
            
            // Create telemetry CSV file
            val telemetryFile = File(filesDir, "telemetry_$timestamp.csv")
            telemetryWriter = FileWriter(telemetryFile)
            telemetryWriter?.appendLine(TELEMETRY_HEADER)
            
            // Create video metadata CSV file
            val videoMetadataFile = File(filesDir, "video_metadata_$timestamp.csv")
            videoMetadataWriter = FileWriter(videoMetadataFile)
            videoMetadataWriter?.appendLine(VIDEO_METADATA_HEADER)
            
            Log.i(TAG, "CSV files initialized: ${telemetryFile.name}, ${videoMetadataFile.name}")
            
        } catch (e: IOException) {
            Log.e(TAG, "Failed to initialize CSV files", e)
            throw e
        }
    }
    
    private fun startDataCapture() {
        try {
            // Start sensor data collection
            val success = sensorManager.registerListener(
                this,
                accelerometer,
                SensorManager.SENSOR_DELAY_GAME // ~50Hz, but we'll filter to 100Hz
            )
            
            if (!success) {
                throw RuntimeException("Failed to register accelerometer listener")
            }
            
            // Start camera recording
            startCameraRecording()
            
            Log.i(TAG, "Data capture started successfully")
            
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start data capture", e)
            stopSelf()
        }
    }
    
    private fun startCameraRecording() {
        try {
            val cameraProviderFuture = ProcessCameraProvider.getInstance(this)
            cameraProviderFuture.addListener({
                cameraProvider = cameraProviderFuture.get()
                
                // Setup camera selector (use back camera)
                val cameraSelector = CameraSelector.DEFAULT_BACK_CAMERA
                
                // Setup preview (optional, for debugging)
                val preview = Preview.Builder().build()
                
                // Setup video capture
                val recorder = Recorder.Builder()
                    .setTargetVideoEncodingBitrate(3_000_000)
                    .build()
                videoCapture = VideoCapture.withOutput(recorder)
                
                // Unbind any previous use cases
                cameraProvider.unbindAll()
                
                // Bind use cases to camera
                cameraProvider.bindToLifecycle(
                    this,
                    cameraSelector,
                    preview,
                    videoCapture
                )
                
                // Start video recording
                startVideoRecording()
                
            }, ContextCompat.getMainExecutor(this))
            
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start camera recording", e)
            throw e
        }
    }
    
    private fun startVideoRecording() {
        try {
            val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(Date())
            val videoFile = File(getExternalFilesDir(null), "fusion_video_$timestamp.mp4")
            
            val outputOptions = FileOutputOptions.Builder(videoFile).build()
            
            currentRecording = videoCapture?.output?.startRecording(outputOptions, cameraExecutor) { event ->
                when (event) {
                    is VideoRecordEvent.Start -> {
                        Log.i(TAG, "Video recording started: ${videoFile.name}")
                        frameCounter = 0L
                    }
                    is VideoRecordEvent.Finalize -> {
                        if (event.hasError()) {
                            Log.e(TAG, "Video recording error: ${event.cause}")
                        } else {
                            Log.i(TAG, "Video recording completed: ${videoFile.name}")
                        }
                    }
                    is VideoRecordEvent.Status -> {
                        // Log recording statistics periodically
                        if (frameCounter % (VIDEO_FPS * 10) == 0L) { // Every 10 seconds
                            Log.d(TAG, "Recording stats: ${event.recordingStats}")
                        }
                    }
                }
            }
            
            // Setup frame analysis for metadata capture
            setupFrameAnalysis()
            
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start video recording", e)
            throw e
        }
    }
    
    private fun setupFrameAnalysis() {
        // This would require ImageAnalysis use case for frame timestamping
        // For now, we'll simulate frame metadata based on recording time
        Thread {
            while (currentRecording?.isActive == true) {
                try {
                    val currentTime = System.nanoTime()
                    
                    // Write frame metadata to CSV
                    videoMetadataWriter?.appendLine("$currentTime,$frameCounter")
                    
                    frameCounter++
                    
                    // Sleep to maintain ~30 FPS timing
                    Thread.sleep(1000L / VIDEO_FPS)
                    
                } catch (e: InterruptedException) {
                    Log.d(TAG, "Frame analysis thread interrupted")
                    break
                } catch (e: Exception) {
                    Log.e(TAG, "Error in frame analysis", e)
                }
            }
        }.start()
    }
    
    override fun onSensorChanged(event: SensorEvent?) {
        if (event?.sensor?.type == Sensor.TYPE_ACCELEROMETER) {
            try {
                val currentTime = System.nanoTime()
                
                // Check for sensor event timing consistency
                if (lastSensorEventTime > 0) {
                    val interval = currentTime - lastSensorEventTime
                    val expectedInterval = 1_000_000_000L / SAMPLE_RATE_HZ.toLong()
                    
                    if (interval > expectedInterval * 1.5) {
                        missedSensorEvents++
                        Log.w(TAG, "Sensor jitter detected: ${interval}ns (expected ~${expectedInterval}ns), missed events: $missedSensorEvents")
                    }
                }
                
                lastSensorEventTime = currentTime
                sensorEventCount++
                
                // Process accelerometer data through native filter
                val filteredData = nativeFilter.processSensorData(
                    event.values[0],
                    event.values[1],
                    event.values[2]
                )
                
                filteredData?.let {
                    // Write filtered telemetry to CSV
                    telemetryWriter?.appendLine("$currentTime,${it[0]},${it[1]},${it[2]}")
                    
                    // Log filtered data periodically (every 100 samples to avoid spam)
                    if (sensorEventCount % 100 == 0L) {
                        Log.d(TAG, String.format("Filtered telemetry #%d: (%.3f, %.3f, %..3f) @ %d", 
                            sensorEventCount, it[0], it[1], it[2], currentTime))
                    }
                }
                
            } catch (e: Exception) {
                Log.e(TAG, "Error processing sensor data", e)
            }
        }
    }
    
    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {
        Log.d(TAG, "Sensor accuracy changed: ${sensor?.name} -> $accuracy")
    }
    
    private fun stopDataCapture() {
        try {
            // Stop sensor data collection
            sensorManager.unregisterListener(this)
            
            // Stop video recording
            currentRecording?.stop()
            
            Log.i(TAG, "Data capture stopped")
            
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping data capture", e)
        }
    }
    
    private fun cleanup() {
        try {
            // Cleanup native filter
            nativeFilter.destroyFilter()
            
            // Close CSV writers
            telemetryWriter?.close()
            videoMetadataWriter?.close()
            
            // Shutdown camera executor
            cameraExecutor.shutdown()
            
            // Log final statistics
            Log.i(TAG, "Final statistics - Sensor events: $sensorEventCount, Missed events: $missedSensorEvents, Frames: $frameCounter")
            
        } catch (e: Exception) {
            Log.e(TAG, "Error during cleanup", e)
        }
    }
    
    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Fusion Data Service",
                NotificationManager.IMPORTANCE_DEFAULT
            ).apply {
                description = "Captures accelerometer data and video for fusion analysis"
            }
            
            val notificationManager = getSystemService(NotificationManager::class.java)
            notificationManager.createNotificationChannel(channel)
        }
    }
    
    private fun createNotification(): Notification {
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Fusion Data Service")
            .setContentText("Capturing sensor data and video...")
            .setSmallIcon(android.R.drawable.ic_menu_camera) // Replace with app icon
            .setOngoing(true)
            .build()
    }
    
    override fun onBind(intent: Intent): IBinder? {
        return null
    }
}

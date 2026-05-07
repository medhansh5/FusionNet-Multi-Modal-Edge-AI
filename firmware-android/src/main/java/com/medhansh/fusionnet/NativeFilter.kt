package com.medhansh.fusionnet

import android.util.Log

/**
 * NativeFilter acts as a bridge to the native fusion-lib library
 * Provides JNI interface to the ButterworthFilter C++ implementation
 */
class NativeFilter {
    private var filterPtr: Long = 0
    private var isInitialized = false
    
    companion object {
        private const val TAG = "NativeFilter"
        
        // Load the native library
        init {
            try {
                System.loadLibrary("fusion-lib")
                Log.d(TAG, "Successfully loaded fusion-lib library")
            } catch (e: UnsatisfiedLinkError) {
                Log.e(TAG, "Failed to load fusion-lib library", e)
                throw e
            }
        }
    }
    
    /**
     * Initialize the ButterworthFilter with specified parameters
     * @param sampleRate Sampling frequency in Hz
     * @param cutoffFreq Cutoff frequency in Hz
     * @return true if initialization successful, false otherwise
     */
    fun initFilter(sampleRate: Float, cutoffFreq: Float): Boolean {
        return try {
            if (isInitialized) {
                Log.w(TAG, "Filter already initialized, destroying previous instance")
                destroyFilter()
            }
            
            filterPtr = initFilter(sampleRate, cutoffFreq)
            isInitialized = filterPtr != 0L
            
            if (isInitialized) {
                Log.i(TAG, "Filter initialized successfully with sampleRate=$sampleRate, cutoffFreq=$cutoffFreq")
            } else {
                Log.e(TAG, "Filter initialization failed")
            }
            
            isInitialized
        } catch (e: Exception) {
            Log.e(TAG, "Exception during filter initialization", e)
            false
        }
    }
    
    /**
     * Process 3-axis accelerometer data through the filter
     * @param x Raw X-axis accelerometer value
     * @param y Raw Y-axis accelerometer value
     * @param z Raw Z-axis accelerometer value
     * @return FloatArray containing filtered [x, y, z] values, or null if error
     */
    fun processSensorData(x: Float, y: Float, z: Float): FloatArray? {
        return try {
            if (!isInitialized) {
                Log.e(TAG, "Filter not initialized, cannot process data")
                return null
            }
            
            val result = processSensorData(filterPtr, x, y, z)
            
            if (result == null) {
                Log.w(TAG, "Native processing returned null")
            } else {
                Log.v(TAG, String.format("Processed: (%.3f, %.3f, %.3f) -> (%.3f, %.3f, %.3f)", 
                    x, y, z, result[0], result[1], result[2]))
            }
            
            result
        } catch (e: Exception) {
            Log.e(TAG, "Exception during sensor data processing", e)
            null
        }
    }
    
    /**
     * Reset filter state buffers
     */
    fun resetFilter() {
        try {
            if (!isInitialized) {
                Log.w(TAG, "Filter not initialized, cannot reset")
                return
            }
            
            resetFilter(filterPtr)
            Log.d(TAG, "Filter reset successfully")
        } catch (e: Exception) {
            Log.e(TAG, "Exception during filter reset", e)
        }
    }
    
    /**
     * Get filter information
     * @return String with filter information
     */
    fun getFilterInfo(): String {
        return try {
            if (!isInitialized) {
                return "Filter not initialized"
            }
            
            getFilterInfo(filterPtr)
        } catch (e: Exception) {
            Log.e(TAG, "Exception getting filter info", e)
            "Error getting filter info: ${e.message}"
        }
    }
    
    /**
     * Destroy the native filter instance and free memory
     */
    fun destroyFilter() {
        try {
            if (isInitialized && filterPtr != 0L) {
                destroyFilter(filterPtr)
                Log.d(TAG, "Filter destroyed successfully")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Exception during filter destruction", e)
        } finally {
            filterPtr = 0L
            isInitialized = false
        }
    }
    
    /**
     * Check if filter is initialized
     */
    fun isInitialized(): Boolean = isInitialized
    
    // Native method declarations
    private external fun initFilter(sampleRate: Float, cutoffFreq: Float): Long
    private external fun processSensorData(filterPtr: Long, x: Float, y: Float, z: Float): FloatArray?
    private external fun destroyFilter(filterPtr: Long)
    private external fun resetFilter(filterPtr: Long)
    private external fun getFilterInfo(filterPtr: Long): String
}

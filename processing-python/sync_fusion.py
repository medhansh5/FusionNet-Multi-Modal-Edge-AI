"""
FusionNet Sensor Fusion Module
================================

Real-time sensor fusion framework for synchronizing 100Hz filtered IMU data 
with 30 FPS video frames using continuous linear interpolation and Bayesian 
late fusion.

Designed for Ghaziabad, India road conditions with Royal Enfield Classic 350
engine vibration filtering (25-41.67 Hz harmonics removal).
"""

import numpy as np
import pandas as pd
from typing import List, Tuple, Optional, Dict, Any
from dataclasses import dataclass
from datetime import datetime, timedelta
import logging
from scipy import interpolate
from collections import deque
import json
import matplotlib.pyplot as plt
import os

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

@dataclass
class IMUSample:
    """IMU data sample with timestamp and filtered acceleration."""
    timestamp: float  # Unix timestamp in seconds
    z_acceleration: float  # Filtered Z-axis acceleration (m/s²)
    confidence: float  # Confidence score [0, 1]
    
@dataclass
class VideoFrame:
    """Video frame with timestamp and hazard detection data."""
    timestamp: float  # Unix timestamp in seconds
    frame_id: int
    hazard_confidence: float  # Vision-based hazard confidence [0, 1]
    
@dataclass
class FusedResult:
    """Fused sensor result with hazard probability."""
    timestamp: float
    frame_id: int
    imu_confidence: float
    vision_confidence: float
    hazard_probability: float
    interpolated_acceleration: float

class ContinuousLinearInterpolator:
    """
    Continuous linear interpolation for aligning 100Hz IMU data with 30 FPS video.
    
    Uses piecewise linear interpolation with boundary handling for real-time
    sensor synchronization.
    """
    
    def __init__(self, max_buffer_size: int = 1000):
        self.imu_buffer = deque(maxlen=max_buffer_size)
        self.interpolator = None
        self.last_timestamp = None
        
    def add_imu_sample(self, sample: IMUSample) -> None:
        """Add IMU sample to buffer and update interpolator."""
        self.imu_buffer.append(sample)
        self.last_timestamp = sample.timestamp
        self._update_interpolator()
        
    def _update_interpolator(self) -> None:
        """Update the linear interpolator with current buffer data."""
        if len(self.imu_buffer) < 2:
            return
            
        # Extract timestamps and accelerations
        timestamps = [s.timestamp for s in self.imu_buffer]
        accelerations = [s.z_acceleration for s in self.imu_buffer]
        
        # Create piecewise linear interpolator
        self.interpolator = interpolate.interp1d(
            timestamps, 
            accelerations, 
            kind='linear', 
            bounds_error=False, 
            fill_value='extrapolate'
        )
        
    def interpolate_at_timestamp(self, timestamp: float) -> Tuple[float, float]:
        """
        Interpolate acceleration at given timestamp.
        
        Returns:
            Tuple of (interpolated_acceleration, confidence)
        """
        if self.interpolator is None or len(self.imu_buffer) < 2:
            return 0.0, 0.0
            
        # Check if timestamp is within reasonable range
        if self.last_timestamp and timestamp > self.last_timestamp + 1.0:
            logger.warning(f"Timestamp far in future: {timestamp}")
            return 0.0, 0.0
            
        try:
            interpolated_accel = float(self.interpolator(timestamp))
            
            # Calculate confidence based on distance to nearest samples
            confidence = self._calculate_interpolation_confidence(timestamp)
            
            return interpolated_accel, confidence
            
        except Exception as e:
            logger.error(f"Interpolation error: {e}")
            return 0.0, 0.0
            
    def _calculate_interpolation_confidence(self, timestamp: float) -> float:
        """
        Calculate interpolation confidence based on temporal proximity to samples.
        """
        if len(self.imu_buffer) < 2:
            return 0.0
            
        # Find nearest samples
        timestamps = np.array([s.timestamp for s in self.imu_buffer])
        
        # Find indices of samples surrounding the timestamp
        left_idx = np.searchsorted(timestamps, timestamp) - 1
        right_idx = left_idx + 1
        
        if left_idx < 0:
            return 0.3  # Low confidence for extrapolation
        if right_idx >= len(timestamps):
            return 0.3
            
        # Calculate temporal distance
        left_time = timestamps[left_idx]
        right_time = timestamps[right_idx]
        total_interval = right_time - left_time
        
        if total_interval <= 0:
            return 0.5
            
        # Higher confidence for smaller interpolation intervals
        # Maximum confidence for intervals < 20ms (typical for 100Hz sampling)
        if total_interval <= 0.02:
            return 0.95
        elif total_interval <= 0.05:
            return 0.85
        elif total_interval <= 0.1:
            return 0.7
        else:
            return 0.5

class BayesianLateFusion:
    """
    Bayesian late fusion matrix for combining IMU and vision hazard probabilities.
    
    Implements: P_hazard = 1 - (1 - w_v * C_vision)(1 - w_i * C_IMU)
    """
    
    def __init__(self, vision_weight: float = 0.6, imu_weight: float = 0.4):
        """
        Initialize fusion weights.
        
        Args:
            vision_weight: Weight for vision-based confidence (default: 0.6)
            imu_weight: Weight for IMU-based confidence (default: 0.4)
        """
        self.vision_weight = vision_weight
        self.imu_weight = imu_weight
        
        # Validate weights
        total_weight = vision_weight + imu_weight
        if abs(total_weight - 1.0) > 0.001:
            logger.warning(f"Weights sum to {total_weight}, normalizing...")
            self.vision_weight = vision_weight / total_weight
            self.imu_weight = imu_weight / total_weight
            
        logger.info(f"Fusion weights - Vision: {self.vision_weight:.2f}, IMU: {self.imu_weight:.2f}")
        
    def fuse_probabilities(self, vision_confidence: float, imu_confidence: float) -> float:
        """
        Fuse vision and IMU confidences using Bayesian late fusion.
        
        Args:
            vision_confidence: Vision-based hazard confidence [0, 1]
            imu_confidence: IMU-based hazard confidence [0, 1]
            
        Returns:
            Combined hazard probability [0, 1]
        """
        # Clamp inputs to valid range
        vision_confidence = np.clip(vision_confidence, 0.0, 1.0)
        imu_confidence = np.clip(imu_confidence, 0.0, 1.0)
        
        # Apply weights
        weighted_vision = self.vision_weight * vision_confidence
        weighted_imu = self.imu_weight * imu_confidence
        
        # Bayesian late fusion formula
        hazard_probability = 1.0 - (1.0 - weighted_vision) * (1.0 - weighted_imu)
        
        return float(hazard_probability)
        
    def update_weights(self, vision_weight: float, imu_weight: float) -> None:
        """Update fusion weights dynamically."""
        self.vision_weight = vision_weight
        self.imu_weight = imu_weight
        
        # Normalize if needed
        total_weight = vision_weight + imu_weight
        if abs(total_weight - 1.0) > 0.001:
            self.vision_weight = vision_weight / total_weight
            self.imu_weight = imu_weight / total_weight

class SyncFusionProcessor:
    """
    Main processor for synchronizing IMU and video data with Bayesian fusion.
    
    Handles real-time processing of 100Hz filtered IMU data and 30 FPS video frames
    for Ghaziabad road hazard detection.
    """
    
    def __init__(self, 
                 vision_weight: float = 0.6, 
                 imu_weight: float = 0.4,
                 buffer_size: int = 1000):
        """
        Initialize the synchronization and fusion processor.
        
        Args:
            vision_weight: Weight for vision-based hazard detection
            imu_weight: Weight for IMU-based hazard detection  
            buffer_size: Maximum buffer size for IMU data
        """
        self.interpolator = ContinuousLinearInterpolator(buffer_size)
        self.fusion_engine = BayesianLateFusion(vision_weight, imu_weight)
        
        # Processing statistics
        self.processed_frames = 0
        self.processed_imu_samples = 0
        self.fusion_results = deque(maxlen=1000)
        
        # Ghaziabad-specific parameters
        self.pothole_threshold = 2.5  # m/s² filtered acceleration threshold
        self.speed_bump_threshold = 5.0  # m/s² for speed bumps
        
        logger.info("SyncFusionProcessor initialized for Ghaziabad road conditions")
        
    def add_imu_sample(self, timestamp: float, z_acceleration: float, confidence: float = 1.0) -> None:
        """Add filtered IMU sample to processor."""
        sample = IMUSample(timestamp, z_acceleration, confidence)
        self.interpolator.add_imu_sample(sample)
        self.processed_imu_samples += 1
        
    def process_video_frame(self, timestamp: float, frame_id: int, hazard_confidence: float) -> Optional[FusedResult]:
        """
        Process video frame with synchronized IMU data.
        
        Args:
            timestamp: Video frame timestamp
            frame_id: Sequential frame identifier
            hazard_confidence: Vision-based hazard detection confidence
            
        Returns:
            FusedResult with combined hazard probability
        """
        # Interpolate IMU data at video timestamp
        interpolated_accel, imu_confidence = self.interpolator.interpolate_at_timestamp(timestamp)
        
        # Calculate IMU-based hazard confidence
        imu_hazard_confidence = self._calculate_imu_hazard_confidence(
            interpolated_accel, imu_confidence
        )
        
        # Fuse with vision confidence using Bayesian fusion
        hazard_probability = self.fusion_engine.fuse_probabilities(
            hazard_confidence, imu_hazard_confidence
        )
        
        # Create fused result
        result = FusedResult(
            timestamp=timestamp,
            frame_id=frame_id,
            imu_confidence=imu_hazard_confidence,
            vision_confidence=hazard_confidence,
            hazard_probability=hazard_probability,
            interpolated_acceleration=interpolated_accel
        )
        
        self.fusion_results.append(result)
        self.processed_frames += 1
        
        return result
        
    def _calculate_imu_hazard_confidence(self, acceleration: float, interpolation_confidence: float) -> float:
        """
        Calculate IMU-based hazard confidence from filtered acceleration.
        
        Designed for Ghaziabad road conditions:
        - Potholes: sudden negative acceleration (impact)
        - Speed bumps: moderate positive acceleration
        - Engine vibrations: filtered out by Butterworth filter
        """
        # Base confidence from interpolation quality
        base_confidence = interpolation_confidence
        
        # Hazard detection based on acceleration magnitude
        if abs(acceleration) < self.pothole_threshold:
            # No significant acceleration detected
            hazard_confidence = 0.1
        elif abs(acceleration) < self.speed_bump_threshold:
            # Moderate acceleration - possible minor hazard
            hazard_confidence = 0.3 + 0.4 * (abs(acceleration) - self.pothole_threshold) / \
                              (self.speed_bump_threshold - self.pothole_threshold)
        else:
            # High acceleration - significant hazard
            hazard_confidence = 0.7 + 0.3 * min(1.0, (abs(acceleration) - self.speed_bump_threshold) / 5.0)
            
        # Combine with interpolation confidence
        final_confidence = hazard_confidence * base_confidence
        
        return np.clip(final_confidence, 0.0, 1.0)
        
    def get_recent_results(self, max_count: int = 100) -> List[FusedResult]:
        """Get most recent fusion results."""
        return list(self.fusion_results)[-max_count:]
        
    def get_statistics(self) -> Dict[str, Any]:
        """Get processing statistics."""
        if self.processed_frames == 0:
            return {
                'processed_frames': 0,
                'processed_imu_samples': self.processed_imu_samples,
                'average_hazard_probability': 0.0,
                'fusion_weights': {
                    'vision': self.fusion_engine.vision_weight,
                    'imu': self.fusion_engine.imu_weight
                }
            }
            
        recent_results = self.get_recent_results(100)
        avg_hazard_prob = np.mean([r.hazard_probability for r in recent_results]) if recent_results else 0.0
        
        return {
            'processed_frames': self.processed_frames,
            'processed_imu_samples': self.processed_imu_samples,
            'average_hazard_probability': avg_hazard_prob,
            'fusion_weights': {
                'vision': self.fusion_engine.vision_weight,
                'imu': self.fusion_engine.imu_weight
            },
            'ghaziabad_thresholds': {
                'pothole': self.pothole_threshold,
                'speed_bump': self.speed_bump_threshold
            }
        }
        
    def export_results(self, filename: str) -> None:
        """Export fusion results to JSON file."""
        results_data = []
        for result in self.fusion_results:
            results_data.append({
                'timestamp': result.timestamp,
                'frame_id': result.frame_id,
                'imu_confidence': result.imu_confidence,
                'vision_confidence': result.vision_confidence,
                'hazard_probability': result.hazard_probability,
                'interpolated_acceleration': result.interpolated_acceleration
            })
            
        export_data = {
            'export_timestamp': datetime.now().isoformat(),
            'statistics': self.get_statistics(),
            'results': results_data
        }
        
        with open(filename, 'w') as f:
            json.dump(export_data, f, indent=2)
            
        logger.info(f"Exported {len(results_data)} fusion results to {filename}")

# Legacy compatibility functions
def synchronize_telemetry(telemetry_path, video_meta_path, output_path):
    """
    Legacy function for backward compatibility.
    Uses the new SyncFusionProcessor for enhanced functionality.
    """
    print("Loading datasets with enhanced FusionNet processor...")
    
    # Load the CSVs generated by the Android Service
    try:
        df_imu = pd.read_csv(telemetry_path)
        df_vid = pd.read_csv(video_meta_path)
    except FileNotFoundError as e:
        print(f"Error: {e}. Please ensure your CSV files are in the data directory.")
        return None, None

    # Sort both datasets by timestamp
    df_imu = df_imu.sort_values(by='timestamp_ns')
    df_vid = df_vid.sort_values(by='timestamp_ns')

    # Convert timestamps to relative seconds
    start_time_ns = min(df_imu['timestamp_ns'].min(), df_vid['timestamp_ns'].min())
    df_imu['time_sec'] = (df_imu['timestamp_ns'] - start_time_ns) / 1e9
    df_vid['time_sec'] = (df_vid['timestamp_ns'] - start_time_ns) / 1e9

    print(f"Processing {len(df_imu)} IMU samples and {len(df_vid)} Video frames...")

    # Initialize enhanced processor
    processor = SyncFusionProcessor()
    
    # Add IMU samples to processor
    for _, row in df_imu.iterrows():
        processor.add_imu_sample(row['time_sec'], row['filtered_z'], 0.95)
    
    # Process video frames with fusion
    fusion_results = []
    for _, row in df_vid.iterrows():
        # Simulate vision confidence (in real system, this comes from vision model)
        vision_confidence = 0.2  # Default low confidence
        if 'hazard_confidence' in row:
            vision_confidence = row['hazard_confidence']
            
        result = processor.process_video_frame(
            row['time_sec'], 
            row['frame_index'], 
            vision_confidence
        )
        
        if result:
            fusion_results.append({
                'frame_index': result.frame_id,
                'timestamp_ns': row['timestamp_ns'],
                'time_sec': result.timestamp,
                'sync_z': result.interpolated_acceleration,
                'hazard_probability': result.hazard_probability,
                'imu_confidence': result.imu_confidence,
                'vision_confidence': result.vision_confidence
            })

    # Create enhanced synchronized dataframe
    df_synced = pd.DataFrame(fusion_results)

    # Save to CSV
    df_synced.to_csv(output_path, index=False)
    print(f"Success! Enhanced synchronized data saved to: {output_path}")
    
    # Print fusion statistics
    stats = processor.get_statistics()
    print(f"Fusion Statistics: {stats}")
    
    return df_imu, df_synced

def plot_synchronization(df_imu, df_synced):
    """Enhanced visualization showing fusion results."""
    print("Generating enhanced synchronization plot...")
    
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 10))
    
    # Plot 1: Original IMU and interpolated points
    ax1.plot(df_imu['time_sec'], df_imu['filtered_z'], 
             label='100Hz Filtered IMU (Z-Axis)', color='lightgray', alpha=0.7)
    
    if 'sync_z' in df_synced.columns:
        ax1.scatter(df_synced['time_sec'], df_synced['sync_z'], 
                    color='red', s=15, label='30Hz Interpolated Points', zorder=5)
    
    ax1.set_title('FusionNet: Temporal Alignment of Video Frames to IMU Telemetry')
    ax1.set_ylabel('Filtered Z-Axis Acceleration (m/s²)')
    ax1.legend()
    ax1.grid(True, linestyle='--', alpha=0.5)
    ax1.set_xlim(df_synced['time_sec'].min(), df_synced['time_sec'].min() + 5.0)
    
    # Plot 2: Hazard probability over time
    if 'hazard_probability' in df_synced.columns:
        ax2.plot(df_synced['time_sec'], df_synced['hazard_probability'], 
                label='Hazard Probability', color='orange', linewidth=2)
        ax2.scatter(df_synced['time_sec'], df_synced['hazard_probability'], 
                   color='red', s=20, alpha=0.6, zorder=5)
        ax2.axhline(y=0.5, color='red', linestyle='--', alpha=0.5, label='Hazard Threshold')
        ax2.set_title('FusionNet: Bayesian Late-Fusion Hazard Probability')
        ax2.set_xlabel('Time (Seconds)')
        ax2.set_ylabel('Hazard Probability')
        ax2.set_ylim(0, 1)
        ax2.legend()
        ax2.grid(True, linestyle='--', alpha=0.5)
        ax2.set_xlim(df_synced['time_sec'].min(), df_synced['time_sec'].min() + 5.0)
    
    plt.tight_layout()
    plt.savefig('enhanced_fusionnet_analysis.png', dpi=300)
    print("Enhanced plot saved as 'enhanced_fusionnet_analysis.png'")

# Example usage and testing
def main():
    """Example usage of the SyncFusionProcessor."""
    processor = SyncFusionProcessor()
    
    # Simulate IMU data (100Hz for 10 seconds)
    start_time = datetime.now().timestamp()
    for i in range(1000):  # 1000 samples at 100Hz = 10 seconds
        timestamp = start_time + i * 0.01  # 10ms intervals
        
        # Simulate filtered acceleration with occasional pothole impacts
        if i % 200 == 150:  # Pothole every 2 seconds
            z_accel = -8.0 + np.random.normal(0, 0.5)
        elif i % 300 == 250:  # Speed bump every 3 seconds
            z_accel = 6.0 + np.random.normal(0, 0.3)
        else:
            z_accel = np.random.normal(0, 0.2)  # Normal road noise
            
        processor.add_imu_sample(timestamp, z_accel, 0.95)
    
    # Simulate video frames (30 FPS for 10 seconds)
    for i in range(300):  # 300 frames at 30FPS = 10 seconds
        timestamp = start_time + i * 0.0333  # 33.3ms intervals
        
        # Simulate vision-based hazard detection
        if i % 60 == 45:  # Detect hazard every 2 seconds
            vision_confidence = 0.8 + np.random.normal(0, 0.1)
        else:
            vision_confidence = 0.1 + np.random.normal(0, 0.05)
            
        result = processor.process_video_frame(timestamp, i, vision_confidence)
        
        if result and result.hazard_probability > 0.5:
            logger.info(f"Hazard detected at frame {result.frame_id}: "
                       f"P={result.hazard_probability:.3f}, "
                       f"Accel={result.interpolated_acceleration:.2f} m/s²")
    
    # Print statistics
    stats = processor.get_statistics()
    logger.info(f"Processing complete: {stats}")
    
    # Export results
    processor.export_results("fusion_results.json")

if __name__ == "__main__":
    # Check if running legacy mode or new enhanced mode
    if len(os.sys.argv) > 1 and os.sys.argv[1] == "--legacy":
        # Legacy mode for backward compatibility
        DATA_DIR = "../data/"
        TELEMETRY_CSV = os.path.join(DATA_DIR, "telemetry.csv")
        VIDEO_META_CSV = os.path.join(DATA_DIR, "video_metadata.csv")
        OUTPUT_CSV = os.path.join(DATA_DIR, "synchronized_dataset.csv")

        # Run the pipeline
        original_imu, synced_data = synchronize_telemetry(TELEMETRY_CSV, VIDEO_META_CSV, OUTPUT_CSV)
        
        if original_imu is not None and synced_data is not None:
            plot_synchronization(original_imu, synced_data)
    else:
        # Run enhanced example
        main()
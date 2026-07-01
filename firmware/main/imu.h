// imu.h — QMI8658 wake-on-motion driver. Configures the IMU's hardware motion
// interrupt (GPIO17) to signal user activity for the idle-dim feature.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Probe + configure the QMI8658 for wake-on-motion and install the GPIO17 ISR.
// On motion it calls idle_notify_activity() (from a task, not the ISR).
// Safe to call once at startup; logs a warning and returns if the IMU is
// absent/unresponsive (feature degrades to touch-only wake).
void imu_start(void);

#ifdef __cplusplus
}
#endif

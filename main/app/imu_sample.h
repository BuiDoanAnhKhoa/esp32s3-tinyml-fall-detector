#pragma once

#include <stdint.h>

// Physical units match the MPU driver: acceleration in g, gyro in degrees/s.
// The sequence advances on every acquisition attempt, including failed reads.
typedef struct {
    int64_t timestamp_us;
    uint32_t sequence;
    float acc[3];
    float gyro[3];
} imu_sample_t;

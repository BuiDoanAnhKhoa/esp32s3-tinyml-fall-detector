#pragma once

#include "esp_err.h"
#include "imu_sample.h"

// Optional console output. submit() never waits for the writer.
esp_err_t imu_stream_start(void);
void imu_stream_submit(const imu_sample_t *sample);

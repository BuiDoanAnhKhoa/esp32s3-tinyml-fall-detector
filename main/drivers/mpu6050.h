#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define MPU6050_I2C_ADDR         0x68
#define MPU6050_WHO_AM_I_6050    0x68
#define MPU6050_WHO_AM_I_6500    0x70

// ±8g sensitivity: 4096 LSB/g | ±250 deg/s sensitivity: 131 LSB/(deg/s)
#define ACCEL_SCALE_FACTOR_8G    4096.0f
#define GYRO_SCALE_FACTOR_250DPS 131.0f

typedef struct {
    float acc_x, acc_y, acc_z; // in g (1g = 9.81 m/s^2)
    float gyr_x, gyr_y, gyr_z; // in deg/s
} mpu6050_data_t;

typedef struct {
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
} mpu6050_handle_t;

/**
 * @brief Initialize I2C bus, wake MPU6050, verify WHO_AM_I, and set range to ±8g.
 */
esp_err_t mpu6050_init(mpu6050_handle_t *mpu, gpio_num_t sda_pin, gpio_num_t scl_pin, uint32_t clk_speed_hz);

/**
 * @brief Read 6-axis raw registers in a single burst and convert to physical units.
 */
esp_err_t mpu6050_read_motion(mpu6050_handle_t *mpu, mpu6050_data_t *data);

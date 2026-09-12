#include "mpu6050.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MPU6050_DRIVER";

#define REG_PWR_MGMT_1      0x6B
#define REG_ACCEL_CONFIG    0x1C
#define REG_GYRO_CONFIG     0x1B
#define REG_WHO_AM_I        0x75
#define REG_ACCEL_XOUT_H    0x3B

static inline int16_t combine_bytes(uint8_t high, uint8_t low) {
    return (int16_t)((high << 8) | low);
}

esp_err_t mpu6050_init(mpu6050_handle_t *mpu, gpio_num_t sda_pin, gpio_num_t scl_pin, uint32_t clk_speed_hz) {
    if (!mpu) return ESP_ERR_INVALID_ARG;

    // 1. Configure I2C master bus
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &mpu->bus_handle);
    if (ret != ESP_OK) return ret;

    // 2. Add MPU device
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU6050_I2C_ADDR,
        .scl_speed_hz = clk_speed_hz,
    };
    ret = i2c_master_bus_add_device(mpu->bus_handle, &dev_cfg, &mpu->dev_handle);
    if (ret != ESP_OK) return ret;

    // 3. Verify WHO_AM_I (0x68 for MPU6050, 0x70 for MPU6500)
    uint8_t who_am_i_reg = REG_WHO_AM_I;
    uint8_t chip_id = 0;
    ret = i2c_master_transmit_receive(mpu->dev_handle, &who_am_i_reg, 1, &chip_id, 1, 50);
    if (ret != ESP_OK) return ret;

    if (chip_id != MPU6050_WHO_AM_I_6050 && chip_id != MPU6050_WHO_AM_I_6500) {
        ESP_LOGE(TAG, "Unknown device ID: 0x%02X", chip_id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Device validated (WHO_AM_I = 0x%02X)", chip_id);

    // 4. Wake up device (clear SLEEP bit)
    uint8_t wake_cmd[2] = {REG_PWR_MGMT_1, 0x00};
    ret = i2c_master_transmit(mpu->dev_handle, wake_cmd, sizeof(wake_cmd), 50);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    // 5. Configure Accelerometer range to ±8g (0x10)
    uint8_t accel_cfg_cmd[2] = {REG_ACCEL_CONFIG, 0x10};
    ret = i2c_master_transmit(mpu->dev_handle, accel_cfg_cmd, sizeof(accel_cfg_cmd), 50);
    if (ret != ESP_OK) return ret;

    // Set the gyro range explicitly so conversion remains correct after an
    // ESP32-only reset, even if the sensor was previously configured differently.
    uint8_t gyro_cfg_cmd[2] = {REG_GYRO_CONFIG, 0x00};
    ret = i2c_master_transmit(mpu->dev_handle, gyro_cfg_cmd, sizeof(gyro_cfg_cmd), 50);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "MPU initialized (±8g, ±250 degrees/s)");
    return ESP_OK;
}

esp_err_t mpu6050_read_motion(mpu6050_handle_t *mpu, mpu6050_data_t *data) {
    if (!mpu || !data) return ESP_ERR_INVALID_ARG;

    uint8_t start_reg = REG_ACCEL_XOUT_H;
    uint8_t buffer[14];

    esp_err_t ret = i2c_master_transmit_receive(mpu->dev_handle, &start_reg, 1, buffer, 14, 20);
    if (ret != ESP_OK) return ret;

    data->acc_x = (float)combine_bytes(buffer[0], buffer[1]) / ACCEL_SCALE_FACTOR_8G;
    data->acc_y = (float)combine_bytes(buffer[2], buffer[3]) / ACCEL_SCALE_FACTOR_8G;
    data->acc_z = (float)combine_bytes(buffer[4], buffer[5]) / ACCEL_SCALE_FACTOR_8G;

    data->gyr_x = (float)combine_bytes(buffer[8], buffer[9]) / GYRO_SCALE_FACTOR_250DPS;
    data->gyr_y = (float)combine_bytes(buffer[10], buffer[11]) / GYRO_SCALE_FACTOR_250DPS;
    data->gyr_z = (float)combine_bytes(buffer[12], buffer[13]) / GYRO_SCALE_FACTOR_250DPS;

    return ESP_OK;
}

esp_err_t mpu6050_sleep(mpu6050_handle_t *mpu) {
    if (!mpu) return ESP_ERR_INVALID_ARG;
    uint8_t cmd[2] = {REG_PWR_MGMT_1, 0x40};  // Set SLEEP bit
    return i2c_master_transmit(mpu->dev_handle, cmd, sizeof(cmd), 50);
}

esp_err_t mpu6050_wake(mpu6050_handle_t *mpu) {
    if (!mpu) return ESP_ERR_INVALID_ARG;
    uint8_t cmd[2] = {REG_PWR_MGMT_1, 0x00};  // Clear SLEEP bit
    esp_err_t ret = i2c_master_transmit(mpu->dev_handle, cmd, sizeof(cmd), 50);
    if (ret == ESP_OK) vTaskDelay(pdMS_TO_TICKS(10));
    return ret;
}

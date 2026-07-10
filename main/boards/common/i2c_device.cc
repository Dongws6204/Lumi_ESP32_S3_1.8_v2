#include "i2c_device.h"

#include <esp_err.h>
#include <esp_log.h>

#define TAG "I2cDevice"

I2cDevice::I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr) {
    i2c_device_config_t i2c_device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,

        // Boot stage ưu tiên ổn định, không cần 400k.
        .scl_speed_hz = 100 * 1000,

        .scl_wait_us = 0,
        .flags =
            {
                .disable_ack_check = 0,
            },
    };

    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &i2c_device_cfg, &i2c_device_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Add I2C device 0x%02X failed: %s", addr, esp_err_to_name(err));
        i2c_device_ = nullptr;
        return;
    }

    ESP_LOGI(TAG, "I2C device 0x%02X added", addr);
}

esp_err_t I2cDevice::WriteRegChecked(uint8_t reg, uint8_t value) {
    if (i2c_device_ == nullptr) {
        ESP_LOGE(TAG, "WriteReg 0x%02X failed: device handle is null", reg);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buffer[2] = {reg, value};
    esp_err_t err = i2c_master_transmit(i2c_device_, buffer, sizeof(buffer), 100);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WriteReg reg=0x%02X value=0x%02X failed: %s", reg, value,
                 esp_err_to_name(err));
    }

    return err;
}

esp_err_t I2cDevice::ReadRegChecked(uint8_t reg, uint8_t* value) {
    if (value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *value = 0;

    if (i2c_device_ == nullptr) {
        ESP_LOGE(TAG, "ReadReg 0x%02X failed: device handle is null", reg);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buffer[1] = {0};
    esp_err_t err = i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, 1, 100);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ReadReg reg=0x%02X failed: %s", reg, esp_err_to_name(err));
        return err;
    }

    *value = buffer[0];
    return ESP_OK;
}

// Giữ API cũ để code Axp2101 không vỡ compile.
// Nhưng tuyệt đối không ESP_ERROR_CHECK ở đây nữa.
void I2cDevice::WriteReg(uint8_t reg, uint8_t value) { WriteRegChecked(reg, value); }

uint8_t I2cDevice::ReadReg(uint8_t reg) {
    uint8_t value = 0;
    ReadRegChecked(reg, &value);
    return value;
}

void I2cDevice::ReadRegs(uint8_t reg, uint8_t* buffer, size_t length) {
    if (buffer == nullptr || length == 0) {
        return;
    }

    if (i2c_device_ == nullptr) {
        ESP_LOGE(TAG, "ReadRegs 0x%02X failed: device handle is null", reg);
        memset(buffer, 0, length);
        return;
    }

    esp_err_t err = i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length, 100);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ReadRegs reg=0x%02X len=%u failed: %s", reg, (unsigned)length,
                 esp_err_to_name(err));
        memset(buffer, 0, length);
    }
}
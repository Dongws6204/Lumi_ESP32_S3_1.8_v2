#ifndef I2C_DEVICE_H
#define I2C_DEVICE_H

#include <driver/i2c_master.h>
#include "esp_err.h"  // <-- Thêm dòng này để nhận diện esp_err_t

class I2cDevice {
public:
    I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr);

    // 2 hàm mới bạn vừa thêm (Rất chuẩn!)
    esp_err_t WriteRegChecked(uint8_t reg, uint8_t value);
    esp_err_t ReadRegChecked(uint8_t reg, uint8_t* value);

protected:
    i2c_master_dev_handle_t i2c_device_;

    void WriteReg(uint8_t reg, uint8_t value);
    uint8_t ReadReg(uint8_t reg);
    void ReadRegs(uint8_t reg, uint8_t* buffer, size_t length);
};

#endif  // I2C_DEVICE_H
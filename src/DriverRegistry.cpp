// Maps configured device type names to their compiled-in driver factories.
#include "DriverRegistry.h"

// Creates the driver implementation selected by the configuration type string.
std::unique_ptr<Driver> makeDriver(const std::string& type) {
    if (type == "bmp280") return makeBmp280Driver();
    if (type == "mpu6050") return makeMpu6050Driver();
    if (type == "bh1750") return makeBh1750Driver();
    if (type == "ds3231") return makeDs3231Driver();
    if (type == "bq4050") return makeBq4050Driver();
    if (type == "24c32bn") return make24c32bnDriver();
    if (type == "ssd1306") return makeSsd1306Driver();
    return nullptr;
}

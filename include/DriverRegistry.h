#pragma once
#include "DeviceDriver.h"
#include <memory>
#include <string>

std::unique_ptr<Driver> makeBmp280Driver();
std::unique_ptr<Driver> makeMpu6050Driver();
std::unique_ptr<Driver> makeBh1750Driver();
std::unique_ptr<Driver> makeDs3231Driver();
std::unique_ptr<Driver> makeBq4050Driver();
std::unique_ptr<Driver> makeDriver(const std::string& type);

// Implements raw motion, temperature, and configuration operations for MPU-6050.
#include "DeviceDriver.h"
#include <sstream>
#include <vector>

struct Mpu6050Driver : Driver {
    std::string title() const override { return "MPU-6050 accelerometer/gyroscope"; }
    std::string capabilities() const override { return "Read accelerometer, temperature and gyroscope raw values; write power/gyro/accel config registers"; }

    std::string readStream(Bus& bus, const DeviceConfig& d) override {
        std::vector<BYTE> who, bytes;
        if (!bus.read(d.address, 0x75, 1, 1, who) || !bus.read(d.address, 0x3B, 1, 14, bytes)) return "ERROR: I2C read failed";
        std::ostringstream output;
        output << "who_am_i=" << hex(who[0]) << ", accel_xyz_raw=("
               << signed16(bytes[0], bytes[1]) << "," << signed16(bytes[2], bytes[3]) << "," << signed16(bytes[4], bytes[5])
               << "), temp_raw=" << signed16(bytes[6], bytes[7]) << ", gyro_xyz_raw=("
               << signed16(bytes[8], bytes[9]) << "," << signed16(bytes[10], bytes[11]) << "," << signed16(bytes[12], bytes[13]) << ")";
        return output.str();
    }

    std::vector<DriverAction> actions() const override {
        return {
            {"wake", "Wake device (PWR_MGMT_1=0)", {}, ""},
            {"gyro-config", "Set gyro config (0x1B)", {"Value (0..255, hex accepted)"}, ""},
            {"accel-config", "Set accel config (0x1C)", {"Value (0..255, hex accepted)"}, ""}
        };
    }

    bool executeAction(Bus& bus, const DeviceConfig& d, const std::string& action,
                       const std::vector<std::string>& parameters, std::string& result) override {
        BYTE reg;
        unsigned value = 0;
        if (action == "wake") {
            if (!parameters.empty()) { result = "Wake action takes no parameters"; return false; }
            reg = 0x6B;
        } else if (action == "gyro-config" || action == "accel-config") {
            if (parameters.size() != 1) { result = "Expected one byte value"; return false; }
            unsigned long parsed = 0;
            if (!parseDriverNumber(parameters[0], parsed) || parsed > 255) {
                result = "Invalid byte value";
                return false;
            }
            value = static_cast<unsigned>(parsed);
            reg = action == "gyro-config" ? 0x1B : 0x1C;
        } else {
            result = "Unknown MPU-6050 action";
            return false;
        }
        if (!bus.writeReg(d.address, reg, static_cast<BYTE>(value))) {
            result = "Write failed for " + hex(reg);
            return false;
        }
        result = "Wrote " + hex(reg) + "=" + hex(value);
        return true;
    }
};

std::unique_ptr<Driver> makeMpu6050Driver() { return std::make_unique<Mpu6050Driver>(); }

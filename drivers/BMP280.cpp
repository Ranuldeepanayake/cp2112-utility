// Implements the BMP280 pressure and temperature sensor driver.
#include "DeviceDriver.h"
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

struct Bmp280Driver : Driver {
    std::string title() const override { return "BMP280 pressure/temperature"; }
    std::string capabilities() const override { return "Read raw and compensated temperature/pressure; write control/config registers"; }

    std::string readStream(Bus& bus, const DeviceConfig& d) override {
        std::vector<BYTE> id, calibration, raw;
        if (!bus.read(d.address, 0xD0, 1, 1, id) ||
            !bus.read(d.address, 0x88, 1, 24, calibration) ||
            !bus.read(d.address, 0xF7, 1, 6, raw)) return "ERROR: I2C read failed";

        const auto u16 = [&calibration](size_t offset) -> uint16_t {
            return static_cast<uint16_t>(calibration[offset] |
                                         (static_cast<uint16_t>(calibration[offset + 1]) << 8));
        };
        const auto s16 = [&u16](size_t offset) -> int16_t {
            return static_cast<int16_t>(u16(offset));
        };
        const uint32_t rawPressure = (static_cast<uint32_t>(raw[0]) << 12) |
                                     (static_cast<uint32_t>(raw[1]) << 4) | (raw[2] >> 4);
        const uint32_t rawTemperature = (static_cast<uint32_t>(raw[3]) << 12) |
                                        (static_cast<uint32_t>(raw[4]) << 4) | (raw[5] >> 4);

        // Apply the Bosch BMP280 datasheet compensation equations using the
        // calibration words stored in registers 0x88 through 0x9F.
        const double temperatureVar1 = (static_cast<double>(rawTemperature) / 16384.0 -
                                        static_cast<double>(u16(0)) / 1024.0) * s16(2);
        const double temperatureVar2 = std::pow(static_cast<double>(rawTemperature) / 131072.0 -
                                                static_cast<double>(u16(0)) / 8192.0, 2.0) * s16(4);
        const double fineTemperature = temperatureVar1 + temperatureVar2;
        const double temperatureC = fineTemperature / 5120.0;

        double pressureVar1 = fineTemperature / 2.0 - 64000.0;
        double pressureVar2 = pressureVar1 * pressureVar1 * s16(16) / 32768.0;
        pressureVar2 += pressureVar1 * s16(14) * 2.0;
        pressureVar2 = pressureVar2 / 4.0 + s16(12) * 65536.0;
        pressureVar1 = (s16(10) * pressureVar1 * pressureVar1 / 524288.0 + s16(8) * pressureVar1) / 524288.0;
        pressureVar1 = (1.0 + pressureVar1 / 32768.0) * u16(6);
        if (std::abs(pressureVar1) < 1e-12) return "ERROR: invalid BMP280 pressure calibration";
        double pressurePa = 1048576.0 - rawPressure;
        pressurePa = (pressurePa - pressureVar2 / 4096.0) * 6250.0 / pressureVar1;
        pressureVar1 = s16(22) * pressurePa * pressurePa / 2147483648.0;
        pressureVar2 = pressurePa * s16(20) / 32768.0;
        pressurePa += (pressureVar1 + pressureVar2 + s16(18)) / 16.0;

        std::ostringstream output;
        output << std::fixed << std::setprecision(2)
               << "chip_id=" << hex(id[0])
               << ", pressure_raw=" << rawPressure
               << ", pressure_pa=" << pressurePa
               << ", pressure_hpa=" << (pressurePa / 100.0)
               << ", temperature_raw=" << rawTemperature
               << ", temperature_c=" << temperatureC;
        return output.str();
    }

    std::vector<DriverAction> actions() const override {
        return {
            {"write-ctrl-meas", "Write ctrl_meas register (0xF4)", {"Value (0..255, hex accepted)"}, ""},
            {"write-config", "Write config register (0xF5)", {"Value (0..255, hex accepted)"}, ""}
        };
    }

    bool executeAction(Bus& bus, const DeviceConfig& d, const std::string& action,
                       const std::vector<std::string>& parameters, std::string& result) override {
        if (parameters.size() != 1) { result = "Expected one byte value"; return false; }
        unsigned long value = 0;
        if (!parseDriverNumber(parameters[0], value) || value > 255) {
            result = "Invalid byte value";
            return false;
        }
        BYTE reg;
        if (action == "write-ctrl-meas") reg = 0xF4;
        else if (action == "write-config") reg = 0xF5;
        else { result = "Unknown BMP280 action"; return false; }
        if (!bus.writeReg(d.address, reg, static_cast<BYTE>(value))) {
            result = "Write failed for " + hex(reg);
            return false;
        }
        result = "Wrote " + hex(reg) + "=" + hex(static_cast<unsigned>(value));
        return true;
    }
};

std::unique_ptr<Driver> makeBmp280Driver() { return std::make_unique<Bmp280Driver>(); }

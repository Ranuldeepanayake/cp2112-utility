// Implements the BMP280 pressure and temperature sensor driver.
#include "DeviceDriver.h"
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

struct Bmp280Driver : Driver {
    // Keep configuration state for a live-polling session. Normal mode
    // continuously refreshes the output registers after this one-time setup.
    bool configured_ = false;

    std::string title() const override { return "BMP280 pressure/temperature"; }
    std::string capabilities() const override { return "Read raw and compensated temperature/pressure; write control/config registers"; }
    std::vector<DriverRegister> registers() const override {
        return {{0xD0, "CHIP_ID", "BMP280 chip identification", true, false},
                {0x88, "CALIBRATION", "Temperature and pressure calibration block", true, false},
                {0xF7, "DATA", "Raw pressure and temperature measurement block", true, false},
                {0xF4, "CTRL_MEAS", "Measurement control and operating mode", true, true},
                {0xF5, "CONFIG", "Standby, filter, and interface configuration", true, true}};
    }

    bool readResult(Bus& bus, const DeviceConfig& d, DeviceResult& result) override {
        std::vector<BYTE> id, calibration, status, raw;
        if (!bus.read(d.address, 0xD0, 1, 1, id) ||
            !bus.read(d.address, 0x88, 1, 24, calibration)) {
            result.error = "I2C read failed";
            return false;
        }

        if (id.empty() || id[0] != 0x58) {
            result.error = "unexpected BMP280 chip ID";
            return false;
        }

        // Configure the sensor once per driver session, following the
        // reference AVR implementation: x16 pressure, x2 temperature, and
        // normal mode. Normal mode continuously cycles measurements, so live
        // polling can read fresh output without restarting each conversion.
        if (!configured_) {
            if (!bus.writeReg(d.address, 0xF4, 0x00) ||
                !bus.writeReg(d.address, 0xF5, 0x00) ||
                !bus.writeReg(d.address, 0xF4, 0x57)) {
                result.error = "could not initialize BMP280 normal mode";
                return false;
            }
            configured_ = true;
            // Allow the first normal-mode conversion to complete.
            Sleep(45);
        }

        // Wait until the BMP280 clears the measuring bit in STATUS (0xF3).
        // The bounded loop prevents a faulty device from hanging live polling.
        bool ready = false;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            if (!bus.read(d.address, 0xF3, 1, 1, status) || status.empty()) {
                result.error = "could not read BMP280 conversion status";
                return false;
            }
            if ((status[0] & 0x09) == 0) {
                ready = true;
                break;
            }
            Sleep(1);
        }
        if (!ready) {
            result.error = "BMP280 conversion timed out";
            return false;
        }

        if (!bus.read(d.address, 0xF7, 1, 6, raw)) {
            result.error = "I2C read failed";
            return false;
        }

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
        if (std::abs(pressureVar1) < 1e-12) {
            result.error = "invalid BMP280 pressure calibration";
            return false;
        }
        double pressurePa = 1048576.0 - rawPressure;
        pressurePa = (pressurePa - pressureVar2 / 4096.0) * 6250.0 / pressureVar1;
        pressureVar1 = s16(22) * pressurePa * pressurePa / 2147483648.0;
        pressureVar2 = pressurePa * s16(20) / 32768.0;
        pressurePa += (pressureVar1 + pressureVar2 + s16(18)) / 16.0;

        result.succeeded = true;
        result.add("chip_id", static_cast<uint64_t>(id[0]), {}, static_cast<uint64_t>(id[0]));
        result.add("pressure_pa", pressurePa, "Pa", static_cast<uint64_t>(rawPressure));
        result.add("pressure_hpa", pressurePa / 100.0, "hPa", static_cast<uint64_t>(rawPressure));
        result.add("temperature_c", temperatureC, "C", static_cast<uint64_t>(rawTemperature));
        return true;
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

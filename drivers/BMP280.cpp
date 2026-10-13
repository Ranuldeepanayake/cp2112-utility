#include "DeviceDriver.h"
#include <sstream>
#include <vector>

struct Bmp280Driver : Driver {
    std::string title() const override { return "BMP280 pressure/temperature"; }
    std::string capabilities() const override { return "Read chip ID and raw temperature/pressure; write control/config registers"; }

    std::string readStream(Bus& bus, const DeviceConfig& d) override {
        std::vector<BYTE> id, raw;
        if (!bus.read(d.address, 0xD0, 1, 1, id) || !bus.read(d.address, 0xF7, 1, 6, raw)) return "ERROR: I2C read failed";
        std::ostringstream output;
        output << "chip_id=" << hex(id[0]) << ", pressure_raw="
               << (((unsigned)raw[0] << 12) | ((unsigned)raw[1] << 4) | (raw[2] >> 4))
               << ", temperature_raw="
               << (((unsigned)raw[3] << 12) | ((unsigned)raw[4] << 4) | (raw[5] >> 4))
               << " (raw ADC; compensation requires calibration coefficients)";
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

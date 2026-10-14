// Implements the BH1750 light sensor driver and its supported I2C commands.
#include "DeviceDriver.h"
#include <iomanip>
#include <sstream>
#include <vector>

struct Bh1750Driver : Driver {
    std::string title() const override { return "BH1750 illuminance sensor"; }
    std::string capabilities() const override { return "Trigger high-resolution measurement; read lux; set power and measurement mode"; }

    bool readResult(Bus& bus, const DeviceConfig& d, DeviceResult& result) override {
        if (!bus.writeCommand(d.address, 0x01)) { result.error = "power-on command failed"; return false; }
        if (!bus.writeCommand(d.address, 0x10)) { result.error = "measurement command failed"; return false; }
        Sleep(180);
        std::vector<BYTE> bytes;
        if (!bus.rawRead(d.address, 2, bytes)) { result.error = "I2C read failed"; return false; }
        const unsigned raw = (static_cast<unsigned>(bytes[0]) << 8) | bytes[1];
        result.succeeded = true;
        result.add("illuminance_lux", raw / 1.2, "lux", static_cast<uint64_t>(raw));
        return true;
    }

    std::vector<DriverAction> actions() const override {
        return {
            {"power-on", "Power on", {}, ""},
            {"power-down", "Power down", {}, ""},
            {"reset", "Reset data register", {}, ""},
            {"set-mode", "Set measurement mode", {"Mode (0x10, 0x11, 0x13, 0x20, 0x21 or 0x23)"}, ""}
        };
    }

    bool executeAction(Bus& bus, const DeviceConfig& d, const std::string& action,
                       const std::vector<std::string>& parameters, std::string& result) override {
        BYTE command;
        if (action == "power-on" && parameters.empty()) command = 0x01;
        else if (action == "power-down" && parameters.empty()) command = 0x00;
        else if (action == "reset" && parameters.empty()) command = 0x07;
        else if (action == "set-mode" && parameters.size() == 1) {
            unsigned long mode = 0;
            if (!parseDriverNumber(parameters[0], mode) || mode > 255 ||
                !(mode == 0x10 || mode == 0x11 || mode == 0x13 ||
                  mode == 0x20 || mode == 0x21 || mode == 0x23)) {
                result = "Invalid BH1750 mode";
                return false;
            }
            command = static_cast<BYTE>(mode);
        } else {
            result = "Unknown BH1750 action or invalid parameters";
            return false;
        }
        if (!bus.writeCommand(d.address, command)) {
            result = "BH1750 command failed: " + hex(command);
            return false;
        }
        result = "Sent BH1750 command " + hex(command);
        return true;
    }
};

std::unique_ptr<Driver> makeBh1750Driver() { return std::make_unique<Bh1750Driver>(); }

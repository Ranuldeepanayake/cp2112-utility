// Implements date, time, temperature, and control-register operations for DS3231.
#include "DeviceDriver.h"
#include <iomanip>
#include <sstream>
#include <vector>

struct Ds3231Driver : Driver {
    std::string title() const override { return "DS3231 RTC"; }
    std::string capabilities() const override { return "Read clock/date and temperature; set RTC date/time; write control register"; }

    bool readResult(Bus& bus, const DeviceConfig& d, DeviceResult& result) override {
        std::vector<BYTE> time, temperature;
        if (!bus.read(d.address, 0x00, 1, 7, time) || !bus.read(d.address, 0x11, 1, 2, temperature)) {
            result.error = "I2C read failed";
            return false;
        }
        const unsigned second = bcdToDec(time[0] & 0x7f), minute = bcdToDec(time[1] & 0x7f);
        const unsigned hour = (time[2] & 0x40) ?
            (bcdToDec(time[2] & 0x1f) % 12 + ((time[2] & 0x20) ? 12 : 0)) :
            bcdToDec(time[2] & 0x3f);
        const unsigned day = bcdToDec(time[4] & 0x3f), month = bcdToDec(time[5] & 0x1f);
        const unsigned year = 2000 + bcdToDec(time[6]);
        const int whole = static_cast<int8_t>(temperature[0]);
        const double celsius = whole + ((temperature[1] >> 6) * 0.25);
        std::ostringstream rtc;
        rtc << std::setfill('0') << std::setw(4) << year << "-"
            << std::setw(2) << month << "-" << std::setw(2) << day << " "
            << std::setw(2) << hour << ":" << std::setw(2) << minute << ":"
            << std::setw(2) << second;
        result.succeeded = true;
        result.add("rtc", rtc.str());
        result.add("temperature_c", celsius, "C", static_cast<int64_t>(whole));
        return true;
    }

    std::vector<DriverAction> actions() const override {
        return {
            {"set-time", "Set date/time (24-hour)", {
                "Year (2000..2099)", "Month (1..12)", "Day (1..31)",
                "Hour (0..23)", "Minute (0..59)", "Second (0..59)"
            }, "This changes the RTC's stored date and time."},
            {"write-control", "Write control register (0x0E)", {"Value (0..255, hex accepted)"}, ""}
        };
    }

    bool executeAction(Bus& bus, const DeviceConfig& d, const std::string& action,
                       const std::vector<std::string>& parameters, std::string& result) override {
        if (action == "set-time") {
            if (parameters.size() != 6) { result = "Expected year, month, day, hour, minute, and second"; return false; }
            unsigned values[6]{};
            for (size_t i = 0; i < 6; ++i) {
                unsigned long parsed = 0;
                if (!parseDriverNumber(parameters[i], parsed) || parsed > 9999) {
                    result = "Invalid date/time value";
                    return false;
                }
                values[i] = static_cast<unsigned>(parsed);
            }
            const unsigned year = values[0], month = values[1], day = values[2];
            const unsigned hour = values[3], minute = values[4], second = values[5];
            if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
                day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
                result = "Date/time out of supported range";
                return false;
            }
            const std::vector<BYTE> bytes = {
                0x00, decToBcd(second), decToBcd(minute), decToBcd(hour),
                0x01, decToBcd(day), decToBcd(month), decToBcd(year - 2000)
            };
            if (!bus.writeBytes(d.address, bytes)) { result = "RTC date/time write failed"; return false; }
            result = "RTC date/time write requested";
            return true;
        }
        if (action == "write-control") {
            if (parameters.size() != 1) { result = "Expected one byte value"; return false; }
            unsigned long value = 0;
            if (!parseDriverNumber(parameters[0], value) || value > 255) {
                result = "Invalid control register value";
                return false;
            }
            if (!bus.writeReg(d.address, 0x0E, static_cast<BYTE>(value))) {
                result = "Control register write failed";
                return false;
            }
            result = "Control register written";
            return true;
        }
        result = "Unknown DS3231 action";
        return false;
    }
};

std::unique_ptr<Driver> makeDs3231Driver() { return std::make_unique<Ds3231Driver>(); }

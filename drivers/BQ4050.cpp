// Implements Smart Battery and diagnostic operations for the TI BQ4050 driver.
#include "DeviceDriver.h"
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
constexpr BYTE COMMAND_TEMPERATURE = 0x08;
constexpr BYTE COMMAND_VOLTAGE = 0x09;
constexpr BYTE COMMAND_CURRENT = 0x0a;
constexpr BYTE COMMAND_BATTERY_MODE = 0x03;
constexpr BYTE COMMAND_RELATIVE_SOC = 0x0d;
constexpr BYTE COMMAND_REMAINING_CAPACITY = 0x0f;
constexpr BYTE COMMAND_FULL_CHARGE_CAPACITY = 0x10;
constexpr BYTE COMMAND_BATTERY_STATUS = 0x16;
constexpr BYTE COMMAND_CYCLE_COUNT = 0x17;
constexpr BYTE COMMAND_MANUFACTURER_ACCESS = 0x00;
constexpr BYTE COMMAND_MANUFACTURER_DATA = 0x23;
constexpr unsigned MAC_SAFETY_ALERT = 0x0050;
constexpr unsigned MAC_SAFETY_STATUS = 0x0051;
constexpr unsigned MAC_PF_ALERT = 0x0052;
constexpr unsigned MAC_PF_STATUS = 0x0053;
constexpr unsigned MAC_OPERATION_STATUS = 0x0054;
constexpr unsigned MAC_CHARGING_STATUS = 0x0055;
constexpr unsigned MAC_GAUGING_STATUS = 0x0056;
constexpr unsigned MAC_MANUFACTURING_STATUS = 0x0057;
constexpr unsigned MAC_DA_STATUS_1 = 0x0071;
constexpr unsigned MAC_DA_STATUS_2 = 0x0072;
constexpr unsigned MAC_LED_TOGGLE = 0x002b;
constexpr unsigned MAC_LED_DISPLAY_PRESS = 0x002c;

bool readWord(Bus& bus, const DeviceConfig& d, BYTE command, unsigned& value) {
    std::vector<BYTE> bytes;
    if (!bus.read(d.address, command, 1, 2, bytes) || bytes.size() != 2) return false;
    value = static_cast<unsigned>(bytes[0]) |
            (static_cast<unsigned>(bytes[1]) << 8);
    return true;
}

bool readManufacturerData(Bus& bus, const DeviceConfig& d, unsigned subcommand,
                          size_t dataLength, std::vector<BYTE>& data) {
    const BYTE commandLow = static_cast<BYTE>(subcommand & 0xff);
    const BYTE commandHigh = static_cast<BYTE>((subcommand >> 8) & 0xff);
    if (!bus.writeBytes(d.address, {COMMAND_MANUFACTURER_ACCESS, commandLow, commandHigh})) return false;

    std::vector<BYTE> response;
    if (!bus.read(d.address, COMMAND_MANUFACTURER_DATA, 1,
                  static_cast<unsigned>(dataLength + 1), response) ||
        response.size() != dataLength + 1 ||
        response[0] != dataLength) {
        return false;
    }
    data.assign(response.begin() + 1, response.end());
    return true;
}

bool readManufacturerStatus(Bus& bus, const DeviceConfig& d, unsigned subcommand,
                            unsigned& status) {
    std::vector<BYTE> data;
    if (!readManufacturerData(bus, d, subcommand, 4, data)) return false;
    status = static_cast<unsigned>(data[0]) |
             (static_cast<unsigned>(data[1]) << 8) |
             (static_cast<unsigned>(data[2]) << 16) |
             (static_cast<unsigned>(data[3]) << 24);
    return true;
}

unsigned readLittleEndianWord(const std::vector<BYTE>& data, size_t offset) {
    return static_cast<unsigned>(data[offset]) |
           (static_cast<unsigned>(data[offset + 1]) << 8);
}

double deciKelvinToCelsius(unsigned value) {
    return value / 10.0 - 273.15;
}

bool sendManufacturerCommand(Bus& bus, const DeviceConfig& d, unsigned subcommand) {
    return bus.writeBytes(d.address, {
        COMMAND_MANUFACTURER_ACCESS,
        static_cast<BYTE>(subcommand & 0xff),
        static_cast<BYTE>((subcommand >> 8) & 0xff)
    });
}
}

struct Bq4050Driver : Driver {
    std::string title() const override { return "Texas Instruments BQ4050 battery gauge"; }
    std::string capabilities() const override {
        return "Read SBS telemetry, BQ4050 protection/status flags, per-cell measurements and temperatures; confirmed LED display test commands";
    }

    bool readResult(Bus& bus, const DeviceConfig& d, DeviceResult& result) override {
        unsigned batteryMode = 0, temperature = 0, voltage = 0, current = 0, relativeSoc = 0;
        unsigned remainingCapacity = 0, fullChargeCapacity = 0, batteryStatus = 0, cycleCount = 0;
        if (!readWord(bus, d, COMMAND_BATTERY_MODE, batteryMode) ||
            !readWord(bus, d, COMMAND_TEMPERATURE, temperature) ||
            !readWord(bus, d, COMMAND_VOLTAGE, voltage) ||
            !readWord(bus, d, COMMAND_CURRENT, current) ||
            !readWord(bus, d, COMMAND_RELATIVE_SOC, relativeSoc) ||
            !readWord(bus, d, COMMAND_REMAINING_CAPACITY, remainingCapacity) ||
            !readWord(bus, d, COMMAND_FULL_CHARGE_CAPACITY, fullChargeCapacity) ||
            !readWord(bus, d, COMMAND_BATTERY_STATUS, batteryStatus) ||
            !readWord(bus, d, COMMAND_CYCLE_COUNT, cycleCount)) {
            result.error = "SMBus word read failed";
            return false;
        }

        const double temperatureC = deciKelvinToCelsius(temperature);
        const int currentMa = static_cast<int16_t>(current);
        const bool capacityMode = (batteryMode & 0x8000) != 0;
        const char* capacityUnit = capacityMode ? "10mwh" : "mah";
        std::ostringstream output;
        output << std::fixed << std::setprecision(2)
               << "battery_mode=" << hex(batteryMode, 4)
               << ", temperature_c=" << temperatureC
               << ", voltage_v=" << std::setprecision(3) << voltage / 1000.0
               << ", current_ma=" << currentMa
               << ", relative_soc_percent=" << relativeSoc
               << ", remaining_capacity_" << capacityUnit << "=" << remainingCapacity
               << ", full_charge_capacity_" << capacityUnit << "=" << fullChargeCapacity
               << ", battery_status=" << hex(batteryStatus, 4)
               << ", cycle_count=" << cycleCount;

        result.succeeded = true;
        result.add("battery_mode", static_cast<uint64_t>(batteryMode), {}, static_cast<uint64_t>(batteryMode));
        result.add("temperature_c", temperatureC, "C", static_cast<uint64_t>(temperature));
        result.add("voltage_v", voltage / 1000.0, "V", static_cast<uint64_t>(voltage));
        result.add("current_ma", static_cast<int64_t>(currentMa), "mA", static_cast<int64_t>(currentMa));
        result.add("relative_soc_percent", static_cast<uint64_t>(relativeSoc), "%");
        result.add("remaining_capacity", static_cast<uint64_t>(remainingCapacity), capacityUnit);
        result.add("full_charge_capacity", static_cast<uint64_t>(fullChargeCapacity), capacityUnit);
        result.add("battery_status", static_cast<uint64_t>(batteryStatus), {}, static_cast<uint64_t>(batteryStatus));
        result.add("cycle_count", static_cast<uint64_t>(cycleCount));

        const unsigned statusCommands[] = {
            MAC_SAFETY_ALERT, MAC_SAFETY_STATUS, MAC_PF_ALERT, MAC_PF_STATUS,
            MAC_OPERATION_STATUS, MAC_CHARGING_STATUS, MAC_GAUGING_STATUS,
            MAC_MANUFACTURING_STATUS
        };
        const char* statusNames[] = {
            "safety_alert", "safety_status", "pf_alert", "pf_status",
            "operation_status", "charging_status", "gauging_status",
            "manufacturing_status"
        };
        for (size_t i = 0; i < sizeof(statusCommands) / sizeof(statusCommands[0]); ++i) {
            unsigned status = 0;
            if (!readManufacturerStatus(bus, d, statusCommands[i], status)) {
                result.error = "BQ4050 manufacturer status read failed (" +
                               std::string(statusNames[i]) + ")";
                return false;
            }
            output << ", " << statusNames[i] << "=" << hex(status, 8);
            result.add(statusNames[i], static_cast<uint64_t>(status), {}, static_cast<uint64_t>(status));
        }

        std::vector<BYTE> data;
        if (!readManufacturerData(bus, d, MAC_DA_STATUS_1, 32, data)) {
            result.error = "BQ4050 DAStatus1 read failed";
            return false;
        }
        output << ", cell_voltage_mv=(" << readLittleEndianWord(data, 0)
               << "," << readLittleEndianWord(data, 2)
               << "," << readLittleEndianWord(data, 4)
               << "," << readLittleEndianWord(data, 6) << ")"
               << ", bat_voltage_mv=" << readLittleEndianWord(data, 8)
               << ", pack_voltage_mv=" << readLittleEndianWord(data, 10)
               << ", cell_current_ma=("
               << static_cast<int16_t>(readLittleEndianWord(data, 12)) << ","
               << static_cast<int16_t>(readLittleEndianWord(data, 14)) << ","
               << static_cast<int16_t>(readLittleEndianWord(data, 16)) << ","
               << static_cast<int16_t>(readLittleEndianWord(data, 18)) << ")"
               << ", cell_power_cw=(" << static_cast<int16_t>(readLittleEndianWord(data, 20))
               << "," << static_cast<int16_t>(readLittleEndianWord(data, 22))
               << "," << static_cast<int16_t>(readLittleEndianWord(data, 24))
               << "," << static_cast<int16_t>(readLittleEndianWord(data, 26)) << ")"
               << ", power_cw=" << static_cast<int16_t>(readLittleEndianWord(data, 28))
               << ", average_power_cw=" << static_cast<int16_t>(readLittleEndianWord(data, 30));
        result.add("cell1_voltage_mv", static_cast<uint64_t>(readLittleEndianWord(data, 0)), "mV");
        result.add("cell2_voltage_mv", static_cast<uint64_t>(readLittleEndianWord(data, 2)), "mV");
        result.add("cell3_voltage_mv", static_cast<uint64_t>(readLittleEndianWord(data, 4)), "mV");
        result.add("cell4_voltage_mv", static_cast<uint64_t>(readLittleEndianWord(data, 6)), "mV");
        result.add("pack_voltage_mv", static_cast<uint64_t>(readLittleEndianWord(data, 10)), "mV");

        if (!readManufacturerData(bus, d, MAC_DA_STATUS_2, 14, data)) {
            result.error = "BQ4050 DAStatus2 read failed";
            return false;
        }
        output << ", internal_temperature_c="
               << deciKelvinToCelsius(readLittleEndianWord(data, 0))
               << ", ts1_temperature_c="
               << deciKelvinToCelsius(readLittleEndianWord(data, 2))
               << ", ts2_temperature_c="
               << deciKelvinToCelsius(readLittleEndianWord(data, 4))
               << ", ts3_temperature_c="
               << deciKelvinToCelsius(readLittleEndianWord(data, 6))
               << ", ts4_temperature_c="
               << deciKelvinToCelsius(readLittleEndianWord(data, 8))
               << ", cell_temperature_c="
               << deciKelvinToCelsius(readLittleEndianWord(data, 10))
               << ", fet_temperature_c="
               << deciKelvinToCelsius(readLittleEndianWord(data, 12));
        result.add("internal_temperature_c", deciKelvinToCelsius(readLittleEndianWord(data, 0)), "C");
        result.add("ts1_temperature_c", deciKelvinToCelsius(readLittleEndianWord(data, 2)), "C");
        result.add("ts2_temperature_c", deciKelvinToCelsius(readLittleEndianWord(data, 4)), "C");
        result.add("ts3_temperature_c", deciKelvinToCelsius(readLittleEndianWord(data, 6)), "C");
        result.add("ts4_temperature_c", deciKelvinToCelsius(readLittleEndianWord(data, 8)), "C");
        result.add("cell_temperature_c", deciKelvinToCelsius(readLittleEndianWord(data, 10)), "C");
        result.add("fet_temperature_c", deciKelvinToCelsius(readLittleEndianWord(data, 12)), "C");
        return true;
    }

    std::vector<DriverAction> actions() const override {
        const std::string warning =
            "This is a display test command; it does not control charge/discharge FETs.";
        return {
            {"led-toggle", "Toggle LED display", {}, warning},
            {"led-press", "Simulate LED display button press", {}, warning}
        };
    }

    bool executeAction(Bus& bus, const DeviceConfig& d, const std::string& action,
                       const std::vector<std::string>& parameters, std::string& result) override {
        if (!parameters.empty()) { result = "BQ4050 display actions take no parameters"; return false; }
        unsigned command;
        if (action == "led-toggle") command = MAC_LED_TOGGLE;
        else if (action == "led-press") command = MAC_LED_DISPLAY_PRESS;
        else { result = "Unknown BQ4050 action"; return false; }
        if (!sendManufacturerCommand(bus, d, command)) {
            result = "BQ4050 LED command failed: " + hex(command, 4);
            return false;
        }
        result = "BQ4050 LED command sent: " + hex(command, 4);
        return true;
    }
};

std::unique_ptr<Driver> makeBq4050Driver() {
    return std::make_unique<Bq4050Driver>();
}

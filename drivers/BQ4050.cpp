// Implements Smart Battery and diagnostic operations for the TI BQ4050 driver.
#include "DeviceDriver.h"
#include <vector>

namespace {
constexpr BYTE COMMAND_TEMPERATURE = 0x08;
constexpr BYTE COMMAND_VOLTAGE = 0x09;
constexpr BYTE COMMAND_CURRENT = 0x0a;
constexpr BYTE COMMAND_AVERAGE_CURRENT = 0x0b;
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
constexpr unsigned MAC_SEAL_DEVICE = 0x0030;

// Default factory security keys (SLUU AQ3A §10.5).
constexpr unsigned DEFAULT_UNSEAL_KEY_1     = 0x0414;
constexpr unsigned DEFAULT_UNSEAL_KEY_2     = 0x3672;
constexpr unsigned DEFAULT_FULL_ACCESS_KEY_1 = 0xFFFF;
constexpr unsigned DEFAULT_FULL_ACCESS_KEY_2 = 0xFFFF;

// OPERATION_STATUS bits [9:8] = SEC[1:0] (BQ4050 encoding).
// 01 = Full Access, 10 = Unsealed, 11 = Sealed, 00 = Reserved
static const char* bq4050SecurityModeName(unsigned sec) {
    switch (sec) {
        case 0x1: return "Full Access";
        case 0x2: return "Unsealed";
        case 0x3: return "Sealed";
        default:  return "Reserved";
    }
}

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
    std::vector<DriverRegister> registers() const override {
        return {{0x03, "BATTERY_MODE", "Battery operating and capacity mode", true, false},
                {0x08, "TEMPERATURE", "Battery temperature", true, false},
                {0x09, "VOLTAGE", "Pack voltage", true, false},
                {0x0A, "CURRENT", "Signed pack current", true, false},
                {0x0B, "AVERAGE_CURRENT", "Average signed pack current", true, false},
                {0x0D, "RELATIVE_SOC", "Relative state of charge", true, false},
                {0x0F, "REMAINING_CAPACITY", "Remaining capacity", true, false},
                {0x10, "FULL_CHARGE_CAPACITY", "Full charge capacity", true, false},
                {0x16, "BATTERY_STATUS", "Battery status flags", true, false},
                {0x17, "CYCLE_COUNT", "Charge cycle count", true, false},
                {0x23, "MANUFACTURER_DATA", "Manufacturer subcommand response data", true, true}};
    }

    bool readResult(Bus& bus, const DeviceConfig& d, DeviceResult& result) override {
        unsigned batteryMode = 0, temperature = 0, voltage = 0, current = 0;
        unsigned averageCurrent = 0, relativeSoc = 0;
        unsigned remainingCapacity = 0, fullChargeCapacity = 0, batteryStatus = 0, cycleCount = 0;
        if (!readWord(bus, d, COMMAND_BATTERY_MODE, batteryMode) ||
            !readWord(bus, d, COMMAND_TEMPERATURE, temperature) ||
            !readWord(bus, d, COMMAND_VOLTAGE, voltage) ||
            !readWord(bus, d, COMMAND_CURRENT, current) ||
            !readWord(bus, d, COMMAND_AVERAGE_CURRENT, averageCurrent) ||
            !readWord(bus, d, COMMAND_RELATIVE_SOC, relativeSoc) ||
            !readWord(bus, d, COMMAND_REMAINING_CAPACITY, remainingCapacity) ||
            !readWord(bus, d, COMMAND_FULL_CHARGE_CAPACITY, fullChargeCapacity) ||
            !readWord(bus, d, COMMAND_BATTERY_STATUS, batteryStatus) ||
            !readWord(bus, d, COMMAND_CYCLE_COUNT, cycleCount)) {
            result.error = "SMBus word read failed";
            return false;
        }

        const double temperatureC    = deciKelvinToCelsius(temperature);
        const int    currentMa       = static_cast<int16_t>(current);
        const int    avgCurrentMa    = static_cast<int16_t>(averageCurrent);
        const bool   capacityMode    = (batteryMode & 0x8000) != 0;
        const char*  capacityUnit    = capacityMode ? "10mwh" : "mah";

        result.add("battery_mode", static_cast<uint64_t>(batteryMode), {}, static_cast<uint64_t>(batteryMode));
        result.add("temperature_c", temperatureC, "C", static_cast<uint64_t>(temperature));
        result.add("voltage_v", voltage / 1000.0, "V", static_cast<uint64_t>(voltage));
        result.add("current_ma", static_cast<int64_t>(currentMa), "mA", static_cast<int64_t>(currentMa));
        result.add("average_current_ma", static_cast<int64_t>(avgCurrentMa), "mA");
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
            result.add(statusNames[i], static_cast<uint64_t>(status), {}, static_cast<uint64_t>(status));
        }

        std::vector<BYTE> data;
        if (!readManufacturerData(bus, d, MAC_DA_STATUS_1, 32, data)) {
            result.error = "BQ4050 DAStatus1 read failed";
            return false;
        }
        result.add("cell1_voltage_mv",  static_cast<uint64_t>(readLittleEndianWord(data,  0)), "mV");
        result.add("cell2_voltage_mv",  static_cast<uint64_t>(readLittleEndianWord(data,  2)), "mV");
        result.add("cell3_voltage_mv",  static_cast<uint64_t>(readLittleEndianWord(data,  4)), "mV");
        result.add("cell4_voltage_mv",  static_cast<uint64_t>(readLittleEndianWord(data,  6)), "mV");
        result.add("bat_voltage_mv",    static_cast<uint64_t>(readLittleEndianWord(data,  8)), "mV");
        result.add("pack_voltage_mv",   static_cast<uint64_t>(readLittleEndianWord(data, 10)), "mV");
        result.add("cell1_current_ma",  static_cast<int64_t>(static_cast<int16_t>(readLittleEndianWord(data, 12))), "mA");
        result.add("cell2_current_ma",  static_cast<int64_t>(static_cast<int16_t>(readLittleEndianWord(data, 14))), "mA");
        result.add("cell3_current_ma",  static_cast<int64_t>(static_cast<int16_t>(readLittleEndianWord(data, 16))), "mA");
        result.add("cell4_current_ma",  static_cast<int64_t>(static_cast<int16_t>(readLittleEndianWord(data, 18))), "mA");
        result.add("cell1_power_cw",    static_cast<int64_t>(static_cast<int16_t>(readLittleEndianWord(data, 20))), "cW");
        result.add("cell2_power_cw",    static_cast<int64_t>(static_cast<int16_t>(readLittleEndianWord(data, 22))), "cW");
        result.add("cell3_power_cw",    static_cast<int64_t>(static_cast<int16_t>(readLittleEndianWord(data, 24))), "cW");
        result.add("cell4_power_cw",    static_cast<int64_t>(static_cast<int16_t>(readLittleEndianWord(data, 26))), "cW");
        result.add("power_cw",          static_cast<int64_t>(static_cast<int16_t>(readLittleEndianWord(data, 28))), "cW");
        result.add("average_power_cw",  static_cast<int64_t>(static_cast<int16_t>(readLittleEndianWord(data, 30))), "cW");

        if (!readManufacturerData(bus, d, MAC_DA_STATUS_2, 14, data)) {
            result.error = "BQ4050 DAStatus2 read failed";
            return false;
        }
        result.add("internal_temperature_c", deciKelvinToCelsius(readLittleEndianWord(data,  0)), "C");
        result.add("ts1_temperature_c",      deciKelvinToCelsius(readLittleEndianWord(data,  2)), "C");
        result.add("ts2_temperature_c",      deciKelvinToCelsius(readLittleEndianWord(data,  4)), "C");
        result.add("ts3_temperature_c",      deciKelvinToCelsius(readLittleEndianWord(data,  6)), "C");
        result.add("ts4_temperature_c",      deciKelvinToCelsius(readLittleEndianWord(data,  8)), "C");
        result.add("cell_temperature_c",     deciKelvinToCelsius(readLittleEndianWord(data, 10)), "C");
        result.add("fet_temperature_c",      deciKelvinToCelsius(readLittleEndianWord(data, 12)), "C");
        result.succeeded = true;
        return true;
    }

    std::vector<DriverAction> actions() const override {
        const std::string ledWarning =
            "This is a display test command; it does not control charge/discharge FETs.";
        return {
            {"led-toggle",        "Toggle LED display", {}, ledWarning},
            {"led-press",         "Simulate LED display button press", {}, ledWarning},
            {"security-status",   "Read current security mode from OperationStatus bits [9:8]", {}, {}},
            {"seal",              "Seal the device (MAC 0x0030). Requires UNSEALED or FULL ACCESS mode.", {}, {}},
            {"enter-unsealed",
             "Transition SEALED → UNSEALED by writing the two-word unseal key to ManufacturerAccess(). "
             "Uses keys from config (unseal_key_1/2) if set, otherwise the factory default 0x0414/0x3672. "
             "Pass two hex words as parameters to override for this call only.",
             {"word1 (optional)", "word2 (optional)"}, {}},
            {"enter-full-access",
             "Transition UNSEALED → FULL ACCESS by writing the two-word full access key to ManufacturerAccess(). "
             "Uses keys from config (full_access_key_1/2) if set, otherwise the factory default 0xFFFF/0xFFFF. "
             "Pass two hex words as parameters to override for this call only.",
             {"word1 (optional)", "word2 (optional)"}, {}},
            {"read-mac",
             "Read any MAC subcommand response via ManufacturerAccess (0x00/0x23). "
             "Useful for reading privileged registers after entering Unsealed or Full Access mode. "
             "Example: read-mac 0x0035 8 reads SecurityKeys.",
             {"subcommand (hex, e.g. 0x0035)", "length (bytes, 1–32)"},
             "Only subcommands that return a data payload work here; "
             "command-only subcommands (seal, reset, LED) have dedicated actions."},
        };
    }

    bool executeAction(Bus& bus, const DeviceConfig& d, const std::string& action,
                       const std::vector<std::string>& parameters, std::string& result) override {
        if (action == "led-toggle" || action == "led-press") {
            if (!parameters.empty()) { result = "BQ4050 LED actions take no parameters"; return false; }
            const unsigned command = (action == "led-toggle") ? MAC_LED_TOGGLE : MAC_LED_DISPLAY_PRESS;
            if (!sendManufacturerCommand(bus, d, command)) {
                result = "BQ4050 LED command failed: " + hex(command, 4);
                return false;
            }
            result = "BQ4050 LED command sent: " + hex(command, 4);
            return true;
        }

        if (action == "security-status") {
            if (!parameters.empty()) { result = "BQ4050 security-status takes no parameters"; return false; }
            return readBq4050SecurityStatus(bus, d, result);
        }

        if (action == "seal") {
            if (!parameters.empty()) { result = "BQ4050 seal takes no parameters"; return false; }
            if (!sendManufacturerCommand(bus, d, MAC_SEAL_DEVICE)) {
                result = "BQ4050 seal command failed";
                return false;
            }
            std::string statusStr;
            readBq4050SecurityStatus(bus, d, statusStr);
            result = "BQ4050 seal command sent. " + statusStr;
            return true;
        }

        if (action == "enter-unsealed" || action == "enter-full-access") {
            const bool isFull = (action == "enter-full-access");
            unsigned word1, word2;
            if (parameters.size() == 2) {
                try {
                    word1 = static_cast<unsigned>(std::stoul(parameters[0], nullptr, 0));
                    word2 = static_cast<unsigned>(std::stoul(parameters[1], nullptr, 0));
                } catch (...) {
                    result = "Invalid key words — expected two hex values e.g. 0x0414 0x3672";
                    return false;
                }
            } else if (!parameters.empty()) {
                result = "Expected zero or two parameters (word1 word2)";
                return false;
            } else if (isFull) {
                word1 = (d.fullAccessKey1 != 0) ? d.fullAccessKey1 : DEFAULT_FULL_ACCESS_KEY_1;
                word2 = (d.fullAccessKey2 != 0) ? d.fullAccessKey2 : DEFAULT_FULL_ACCESS_KEY_2;
            } else {
                word1 = (d.unsealKey1 != 0) ? d.unsealKey1 : DEFAULT_UNSEAL_KEY_1;
                word2 = (d.unsealKey2 != 0) ? d.unsealKey2 : DEFAULT_UNSEAL_KEY_2;
            }
            // Write word1 then word2 to ManufacturerAccess — both must arrive within 4 s.
            if (!bus.writeBytes(d.address, {COMMAND_MANUFACTURER_ACCESS,
                    static_cast<BYTE>(word1 & 0xFF), static_cast<BYTE>((word1 >> 8) & 0xFF)}) ||
                !bus.writeBytes(d.address, {COMMAND_MANUFACTURER_ACCESS,
                    static_cast<BYTE>(word2 & 0xFF), static_cast<BYTE>((word2 >> 8) & 0xFF)})) {
                result = "BQ4050 key write failed (key: " + hex(word1, 4) + " / " + hex(word2, 4) + ")";
                return false;
            }
            std::string statusStr;
            readBq4050SecurityStatus(bus, d, statusStr);
            result = "BQ4050 key sequence sent (" + hex(word1, 4) + " / " + hex(word2, 4) + "). " + statusStr;
            return true;
        }

        if (action == "read-mac") {
            if (parameters.size() != 2) {
                result = "read-mac requires two parameters: subcommand (hex) and byte count";
                return false;
            }
            unsigned subcommand, length;
            try {
                subcommand = static_cast<unsigned>(std::stoul(parameters[0], nullptr, 0));
                length     = static_cast<unsigned>(std::stoul(parameters[1], nullptr, 0));
            } catch (...) {
                result = "Invalid parameters — expected e.g. 0x0035 8";
                return false;
            }
            if (length == 0 || length > 32) {
                result = "Length must be 1–32 bytes";
                return false;
            }
            std::vector<BYTE> data;
            if (!readManufacturerData(bus, d, subcommand, length, data)) {
                result = "MAC read failed (subcommand=" + hex(subcommand, 4) +
                         ", length=" + std::to_string(length) + " — verify security mode and subcommand)";
                return false;
            }
            std::string bytes;
            for (size_t i = 0; i < data.size(); ++i) {
                if (i > 0) bytes += " ";
                bytes += hex(static_cast<unsigned>(data[i]), 2);
            }
            result = "MAC " + hex(subcommand, 4) + " [" + std::to_string(length) + "B]: " + bytes;
            if (length == 2) {
                const unsigned w = static_cast<unsigned>(data[0]) | (static_cast<unsigned>(data[1]) << 8);
                result += "  (" + hex(w, 4) + " = " + std::to_string(w) + ")";
            } else if (length == 4) {
                const unsigned dw = static_cast<unsigned>(data[0]) | (static_cast<unsigned>(data[1]) << 8) |
                                    (static_cast<unsigned>(data[2]) << 16) | (static_cast<unsigned>(data[3]) << 24);
                result += "  (" + hex(dw, 8) + " = " + std::to_string(dw) + ")";
            }
            return true;
        }

        result = "Unknown BQ4050 action: " + action;
        return false;
    }

private:
    static bool readBq4050SecurityStatus(Bus& bus, const DeviceConfig& d, std::string& out) {
        std::vector<BYTE> data;
        if (!readManufacturerData(bus, d, MAC_OPERATION_STATUS, 4, data)) {
            out = "Security status: unreadable (OperationStatus MAC read failed)";
            return false;
        }
        const unsigned opStatus = static_cast<unsigned>(data[0])        |
                                  (static_cast<unsigned>(data[1]) << 8)  |
                                  (static_cast<unsigned>(data[2]) << 16) |
                                  (static_cast<unsigned>(data[3]) << 24);
        const unsigned sec = (opStatus >> 8) & 0x3;
        out = "Security mode: " + std::string(bq4050SecurityModeName(sec)) +
              " (SEC[1:0]=" + std::to_string(sec) +
              ", OperationStatus=" + hex(opStatus, 8) + ")";
        return true;
    }
};

std::unique_ptr<Driver> makeBq4050Driver() {
    return std::make_unique<Bq4050Driver>();
}

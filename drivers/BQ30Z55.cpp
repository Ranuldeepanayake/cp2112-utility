// Implements Smart Battery and diagnostic operations for the TI bq30z554-R1 driver.
#include "DeviceDriver.h"
#include <vector>

namespace {
// SBS standard word commands
constexpr BYTE CMD_MANUFACTURER_ACCESS        = 0x00;
constexpr BYTE CMD_BATTERY_MODE               = 0x03;
constexpr BYTE CMD_TEMPERATURE          = 0x08;
constexpr BYTE CMD_VOLTAGE              = 0x09;
constexpr BYTE CMD_CURRENT              = 0x0A;
constexpr BYTE CMD_AVERAGE_CURRENT      = 0x0B;
constexpr BYTE CMD_RELATIVE_SOC         = 0x0D;
constexpr BYTE CMD_REMAINING_CAPACITY   = 0x0F;
constexpr BYTE CMD_FULL_CHARGE_CAPACITY = 0x10;
constexpr BYTE CMD_BATTERY_STATUS       = 0x16;
constexpr BYTE CMD_CYCLE_COUNT          = 0x17;
constexpr BYTE CMD_MANUFACTURER_DATA    = 0x23;

// Direct block SBS status registers (section 10.36-10.43 of SLUUA79)
constexpr BYTE CMD_SAFETY_ALERT         = 0x50;
constexpr BYTE CMD_SAFETY_STATUS        = 0x51;
constexpr BYTE CMD_PF_ALERT             = 0x52;
constexpr BYTE CMD_PF_STATUS            = 0x53;
constexpr BYTE CMD_OPERATION_STATUS     = 0x54;
constexpr BYTE CMD_CHARGING_STATUS      = 0x55;
constexpr BYTE CMD_GAUGING_STATUS       = 0x56;
constexpr BYTE CMD_MANUFACTURING_STATUS = 0x57;

// MAC subcommands for extended data blocks (section 10.1.51-10.1.54 of SLUUA79)
constexpr unsigned MAC_VOLTAGES      = 0x0071; // 12 bytes: cell0-3, BAT, PACK (mV LE16)
constexpr unsigned MAC_TEMPERATURES  = 0x0072; // 14 bytes: int, ts1-4, cell, fet (0.1K LE16)
constexpr unsigned MAC_ITSTATUS1     = 0x0073; // 30 bytes: DOD0, QMAX, StateTime, DODEOC per cell
constexpr unsigned MAC_ITSTATUS2     = 0x0074; // 30 bytes: grid, CompRes, CBTime, RaScale per cell
constexpr unsigned MAC_DEVICE_RESET  = 0x0012;
constexpr unsigned MAC_SEAL_DEVICE   = 0x0030;

// OPERATION_STATUS bits [9:8] = SEC[1:0] (bq30z554-R1 encoding, SLUU511 §10.5).
// 01 = Unsealed, 10 = Full Access, 11 = Sealed, 00 = Reserved
static const char* bq30z55SecurityModeName(unsigned sec) {
    switch (sec) {
        case 0x1: return "Unsealed";
        case 0x2: return "Full Access";
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

// Reads a 4-byte status block directly from a bq30z554-R1 SBS block register.
bool readBlock4(Bus& bus, const DeviceConfig& d, BYTE command, unsigned& value) {
    std::vector<BYTE> response;
    if (!bus.read(d.address, command, 1, 5, response) ||
        response.size() != 5 || response[0] != 4) return false;
    value = static_cast<unsigned>(response[1])        |
            (static_cast<unsigned>(response[2]) << 8)  |
            (static_cast<unsigned>(response[3]) << 16) |
            (static_cast<unsigned>(response[4]) << 24);
    return true;
}

// Writes a MAC subcommand to ManufacturerAccess (0x00) and reads the result
// from ManufacturerData (0x23).
bool readMacData(Bus& bus, const DeviceConfig& d, unsigned subcommand,
                 size_t dataLength, std::vector<BYTE>& data) {
    const BYTE cmdLow  = static_cast<BYTE>(subcommand & 0xff);
    const BYTE cmdHigh = static_cast<BYTE>((subcommand >> 8) & 0xff);
    if (!bus.writeBytes(d.address, {CMD_MANUFACTURER_ACCESS, cmdLow, cmdHigh})) return false;
    std::vector<BYTE> response;
    if (!bus.read(d.address, CMD_MANUFACTURER_DATA, 1,
                  static_cast<unsigned>(dataLength + 1), response) ||
        response.size() != dataLength + 1 ||
        response[0] != dataLength) {
        return false;
    }
    data.assign(response.begin() + 1, response.end());
    return true;
}

unsigned readLEWord(const std::vector<BYTE>& data, size_t offset) {
    return static_cast<unsigned>(data[offset]) |
           (static_cast<unsigned>(data[offset + 1]) << 8);
}

uint32_t readLEWord32(const std::vector<BYTE>& data, size_t offset) {
    return static_cast<uint32_t>(data[offset])          |
           (static_cast<uint32_t>(data[offset + 1]) << 8)  |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

double deciKelvinToCelsius(unsigned value) {
    return value / 10.0 - 273.15;
}
}

struct Bq30z55Driver : Driver {
    std::string title() const override {
        return "Texas Instruments bq30z554-R1 battery pack manager";
    }
    std::string capabilities() const override {
        return "Read SBS telemetry, direct safety/protection/operation status flags, "
               "per-cell voltages and temperatures via MAC Voltages(0x0071) and "
               "Temperatures(0x0072), Impedance Track gauging internals via "
               "ITStatus1(0x0073) and ITStatus2(0x0074); send device reset";
    }
    std::vector<DriverRegister> registers() const override {
        return {
            {0x03, "BATTERY_MODE",         "Battery operating and capacity mode flags",  true, false},
            {0x08, "TEMPERATURE",          "Battery temperature (0.1K)",                 true, false},
            {0x09, "VOLTAGE",              "Pack voltage (mV)",                          true, false},
            {0x0A, "CURRENT",              "Instantaneous signed pack current (mA)",     true, false},
            {0x0B, "AVERAGE_CURRENT",      "Average signed pack current (mA)",           true, false},
            {0x0D, "RELATIVE_SOC",         "Relative state of charge (%)",               true, false},
            {0x0F, "REMAINING_CAPACITY",   "Remaining capacity (mAh or 10mWh)",          true, false},
            {0x10, "FULL_CHARGE_CAPACITY", "Full charge capacity (mAh or 10mWh)",        true, false},
            {0x16, "BATTERY_STATUS",       "SBS battery status flags",                   true, false},
            {0x17, "CYCLE_COUNT",          "Charge cycle count",                         true, false},
            {0x23, "MANUFACTURER_DATA",    "MAC subcommand response block",              true, true},
            {0x50, "SAFETY_ALERT",         "Active safety alert flags (32-bit block)",   true, false},
            {0x51, "SAFETY_STATUS",        "Latched safety status flags (32-bit block)", true, false},
            {0x52, "PF_ALERT",             "Permanent fail alert flags (32-bit block)",  true, false},
            {0x53, "PF_STATUS",            "Permanent fail status flags (32-bit block)", true, false},
            {0x54, "OPERATION_STATUS",     "Device operation status (32-bit block)",     true, false},
            {0x55, "CHARGING_STATUS",      "Charging algorithm status (32-bit block)",   true, false},
            {0x56, "GAUGING_STATUS",       "Impedance Track gauging status (32-bit)",    true, false},
            {0x57, "MANUFACTURING_STATUS", "Manufacturing and test mode status (32-bit)", true, false},
            // ITSTATUS1 (MAC 0x0073) and ITSTATUS2 (MAC 0x0074) are not direct SMBus
            // registers on the bq30z554-R1; they are read via the MAC 0x00/0x23 path
            // in readResult() and must not be addressed directly at 0x73/0x74.
        };
    }

    bool readResult(Bus& bus, const DeviceConfig& d, DeviceResult& result) override {
        unsigned batteryMode = 0, temperature = 0, voltage = 0, current = 0;
        unsigned averageCurrent = 0, relativeSoc = 0;
        unsigned remainingCapacity = 0, fullChargeCapacity = 0;
        unsigned batteryStatus = 0, cycleCount = 0;

        if (!readWord(bus, d, CMD_BATTERY_MODE,         batteryMode)       ||
            !readWord(bus, d, CMD_TEMPERATURE,          temperature)        ||
            !readWord(bus, d, CMD_VOLTAGE,              voltage)            ||
            !readWord(bus, d, CMD_CURRENT,              current)            ||
            !readWord(bus, d, CMD_AVERAGE_CURRENT,      averageCurrent)     ||
            !readWord(bus, d, CMD_RELATIVE_SOC,         relativeSoc)        ||
            !readWord(bus, d, CMD_REMAINING_CAPACITY,   remainingCapacity)  ||
            !readWord(bus, d, CMD_FULL_CHARGE_CAPACITY, fullChargeCapacity) ||
            !readWord(bus, d, CMD_BATTERY_STATUS,       batteryStatus)      ||
            !readWord(bus, d, CMD_CYCLE_COUNT,          cycleCount)) {
            result.error = "SMBus word read failed";
            return false;
        }

        const double temperatureC     = deciKelvinToCelsius(temperature);
        const int    currentMa        = static_cast<int16_t>(current);
        const int    avgCurrentMa     = static_cast<int16_t>(averageCurrent);
        const bool   capacityMode     = (batteryMode & 0x8000) != 0;
        const char*  capacityUnit     = capacityMode ? "10mwh" : "mah";

        result.add("battery_mode",         static_cast<uint64_t>(batteryMode),       {}, static_cast<uint64_t>(batteryMode));
        result.add("temperature_c",        temperatureC,                              "C", static_cast<uint64_t>(temperature));
        result.add("voltage_v",            voltage / 1000.0,                          "V", static_cast<uint64_t>(voltage));
        result.add("current_ma",           static_cast<int64_t>(currentMa),           "mA");
        result.add("average_current_ma",   static_cast<int64_t>(avgCurrentMa),        "mA");
        result.add("relative_soc_percent", static_cast<uint64_t>(relativeSoc),        "%");
        result.add("remaining_capacity",   static_cast<uint64_t>(remainingCapacity),  capacityUnit);
        result.add("full_charge_capacity", static_cast<uint64_t>(fullChargeCapacity), capacityUnit);
        result.add("battery_status",       static_cast<uint64_t>(batteryStatus),      {}, static_cast<uint64_t>(batteryStatus));
        result.add("cycle_count",          static_cast<uint64_t>(cycleCount));

        // Direct block reads for safety and operational status (0x50-0x57)
        constexpr BYTE statusCmds[] = {
            CMD_SAFETY_ALERT, CMD_SAFETY_STATUS, CMD_PF_ALERT, CMD_PF_STATUS,
            CMD_OPERATION_STATUS, CMD_CHARGING_STATUS, CMD_GAUGING_STATUS,
            CMD_MANUFACTURING_STATUS
        };
        const char* statusKeys[] = {
            "safety_alert", "safety_status", "pf_alert", "pf_status",
            "operation_status", "charging_status", "gauging_status",
            "manufacturing_status"
        };
        for (size_t i = 0; i < sizeof(statusCmds) / sizeof(statusCmds[0]); ++i) {
            unsigned status = 0;
            if (!readBlock4(bus, d, statusCmds[i], status)) {
                result.error = std::string("Status register read failed (") +
                               statusKeys[i] + ")";
                return false;
            }
            result.add(statusKeys[i], static_cast<uint64_t>(status), {}, static_cast<uint64_t>(status));
        }

        // MAC 0x0071 Voltages(): 12 bytes
        // cell0, cell1, cell2, cell3, BAT, PACK (each LE16 mV)
        std::vector<BYTE> vData;
        if (!readMacData(bus, d, MAC_VOLTAGES, 12, vData)) {
            result.error = "bq30z554-R1 Voltages() MAC read failed";
            return false;
        }
        result.add("cell1_voltage_mv", static_cast<uint64_t>(readLEWord(vData,  0)), "mV");
        result.add("cell2_voltage_mv", static_cast<uint64_t>(readLEWord(vData,  2)), "mV");
        result.add("cell3_voltage_mv", static_cast<uint64_t>(readLEWord(vData,  4)), "mV");
        result.add("cell4_voltage_mv", static_cast<uint64_t>(readLEWord(vData,  6)), "mV");
        result.add("bat_voltage_mv",   static_cast<uint64_t>(readLEWord(vData,  8)), "mV");
        result.add("pack_voltage_mv",  static_cast<uint64_t>(readLEWord(vData, 10)), "mV");

        // MAC 0x0072 Temperatures(): 14 bytes
        // int, ts1, ts2, ts3, ts4, cell, fet (each LE16 0.1K)
        std::vector<BYTE> tData;
        if (!readMacData(bus, d, MAC_TEMPERATURES, 14, tData)) {
            result.error = "bq30z554-R1 Temperatures() MAC read failed";
            return false;
        }
        result.add("internal_temperature_c", deciKelvinToCelsius(readLEWord(tData,  0)), "C");
        result.add("ts1_temperature_c",      deciKelvinToCelsius(readLEWord(tData,  2)), "C");
        result.add("ts2_temperature_c",      deciKelvinToCelsius(readLEWord(tData,  4)), "C");
        result.add("ts3_temperature_c",      deciKelvinToCelsius(readLEWord(tData,  6)), "C");
        result.add("ts4_temperature_c",      deciKelvinToCelsius(readLEWord(tData,  8)), "C");
        result.add("cell_temperature_c",     deciKelvinToCelsius(readLEWord(tData, 10)), "C");
        result.add("fet_temperature_c",      deciKelvinToCelsius(readLEWord(tData, 12)), "C");

        // MAC 0x0073 ITStatus1(): 30 bytes (section 10.57 of SLUUA79)
        // DOD0_0..3 (I2), ChargeDOD0Update (I2), QMAX0..3 (I2),
        // StateTime (U4), DODEOC0..3 (U2)
        std::vector<BYTE> it1;
        if (!readMacData(bus, d, MAC_ITSTATUS1, 30, it1)) {
            result.error = "bq30z554-R1 ITStatus1() MAC read failed";
            return false;
        }
        result.add("dod0_cell1",           static_cast<int64_t>(static_cast<int16_t>(readLEWord(it1,  0))));
        result.add("dod0_cell2",           static_cast<int64_t>(static_cast<int16_t>(readLEWord(it1,  2))));
        result.add("dod0_cell3",           static_cast<int64_t>(static_cast<int16_t>(readLEWord(it1,  4))));
        result.add("dod0_cell4",           static_cast<int64_t>(static_cast<int16_t>(readLEWord(it1,  6))));
        result.add("charge_dod0_update",   static_cast<int64_t>(static_cast<int16_t>(readLEWord(it1,  8))));
        result.add("qmax_cell1",           static_cast<int64_t>(static_cast<int16_t>(readLEWord(it1, 10))));
        result.add("qmax_cell2",           static_cast<int64_t>(static_cast<int16_t>(readLEWord(it1, 12))));
        result.add("qmax_cell3",           static_cast<int64_t>(static_cast<int16_t>(readLEWord(it1, 14))));
        result.add("qmax_cell4",           static_cast<int64_t>(static_cast<int16_t>(readLEWord(it1, 16))));
        result.add("state_time_s",         static_cast<uint64_t>(readLEWord32(it1, 18)));
        result.add("dodeoc_cell1",         static_cast<uint64_t>(readLEWord(it1, 22)));
        result.add("dodeoc_cell2",         static_cast<uint64_t>(readLEWord(it1, 24)));
        result.add("dodeoc_cell3",         static_cast<uint64_t>(readLEWord(it1, 26)));
        result.add("dodeoc_cell4",         static_cast<uint64_t>(readLEWord(it1, 28)));

        // MAC 0x0074 ITStatus2(): 30 bytes (section 10.58 of SLUUA79)
        // PackGrid (U1), LStatus (U1), CellGrid0..3 (U1 each),
        // CompRes0..3 (U2 each), CBTime0..3 (U2 each), RaScale0..3 (U2 each)
        std::vector<BYTE> it2;
        if (!readMacData(bus, d, MAC_ITSTATUS2, 30, it2)) {
            result.error = "bq30z554-R1 ITStatus2() MAC read failed";
            return false;
        }
        result.add("pack_grid",            static_cast<uint64_t>(it2[0]));
        result.add("learned_status",       static_cast<uint64_t>(it2[1]), {}, static_cast<uint64_t>(it2[1]));
        result.add("cell_grid1",           static_cast<uint64_t>(it2[2]));
        result.add("cell_grid2",           static_cast<uint64_t>(it2[3]));
        result.add("cell_grid3",           static_cast<uint64_t>(it2[4]));
        result.add("cell_grid4",           static_cast<uint64_t>(it2[5]));
        result.add("comp_res_cell1",       static_cast<uint64_t>(readLEWord(it2,  6)));
        result.add("comp_res_cell2",       static_cast<uint64_t>(readLEWord(it2,  8)));
        result.add("comp_res_cell3",       static_cast<uint64_t>(readLEWord(it2, 10)));
        result.add("comp_res_cell4",       static_cast<uint64_t>(readLEWord(it2, 12)));
        result.add("cb_time_cell1",        static_cast<uint64_t>(readLEWord(it2, 14)));
        result.add("cb_time_cell2",        static_cast<uint64_t>(readLEWord(it2, 16)));
        result.add("cb_time_cell3",        static_cast<uint64_t>(readLEWord(it2, 18)));
        result.add("cb_time_cell4",        static_cast<uint64_t>(readLEWord(it2, 20)));
        result.add("ra_scale_cell1",       static_cast<uint64_t>(readLEWord(it2, 22)));
        result.add("ra_scale_cell2",       static_cast<uint64_t>(readLEWord(it2, 24)));
        result.add("ra_scale_cell3",       static_cast<uint64_t>(readLEWord(it2, 26)));
        result.add("ra_scale_cell4",       static_cast<uint64_t>(readLEWord(it2, 28)));

        result.succeeded = true;
        return true;
    }

    std::vector<DriverAction> actions() const override {
        return {
            {"device-reset",
             "Send Device Reset (MAC 0x0012).", {},
             "This sends a firmware reset to the bq30z554-R1. The device will restart "
             "and briefly drop off the bus."},
            {"security-status",
             "Read current security mode from OperationStatus bits [9:8].", {}, {}},
            {"seal",
             "Seal the device (MAC 0x0030). Requires UNSEALED or FULL ACCESS mode.", {}, {}},
            {"read-mac",
             "Read any MAC subcommand response via ManufacturerAccess (0x00/0x23). "
             "Useful for reading privileged registers after entering Unsealed or Full Access mode. "
             "Example: read-mac 0x0035 8 reads SecurityKeys.",
             {"subcommand (hex, e.g. 0x0035)", "length (bytes, 1–32)"},
             "Only subcommands that return a data payload work here; "
             "command-only subcommands (seal, reset) have dedicated actions."},
        };
    }

    bool executeAction(Bus& bus, const DeviceConfig& d, const std::string& action,
                       const std::vector<std::string>& parameters, std::string& result) override {
        if (action == "enter-unsealed" || action == "enter-full-access") {
            result = "bq30z554-R1 uses SHA-1 HMAC authentication for " + action +
                     " — not supported by this driver. See SLUU511 §10.5 and MAC 0x0031/0x0032.";
            return false;
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
            if (!readMacData(bus, d, subcommand, length, data)) {
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

        if (!parameters.empty()) {
            result = "bq30z554-R1 actions take no parameters";
            return false;
        }

        if (action == "device-reset") {
            if (!bus.writeBytes(d.address, {
                    CMD_MANUFACTURER_ACCESS,
                    static_cast<BYTE>(MAC_DEVICE_RESET & 0xff),
                    static_cast<BYTE>((MAC_DEVICE_RESET >> 8) & 0xff)})) {
                result = "bq30z554-R1 device reset command failed";
                return false;
            }
            result = "bq30z554-R1 device reset sent (MAC 0x0012)";
            return true;
        }

        if (action == "seal") {
            if (!bus.writeBytes(d.address, {
                    CMD_MANUFACTURER_ACCESS,
                    static_cast<BYTE>(MAC_SEAL_DEVICE & 0xff),
                    static_cast<BYTE>((MAC_SEAL_DEVICE >> 8) & 0xff)})) {
                result = "bq30z554-R1 seal command failed";
                return false;
            }
            std::string statusStr;
            readBq30z55SecurityStatus(bus, d, statusStr);
            result = "bq30z554-R1 seal command sent (MAC 0x0030). " + statusStr;
            return true;
        }

        if (action == "security-status") {
            return readBq30z55SecurityStatus(bus, d, result);
        }

        result = "Unknown bq30z554-R1 action: " + action;
        return false;
    }

private:
    static bool readBq30z55SecurityStatus(Bus& bus, const DeviceConfig& d, std::string& out) {
        unsigned opStatus = 0;
        if (!readBlock4(bus, d, CMD_OPERATION_STATUS, opStatus)) {
            out = "Security status: unreadable (OperationStatus read failed)";
            return false;
        }
        const unsigned sec = (opStatus >> 8) & 0x3;
        out = "Security mode: " + std::string(bq30z55SecurityModeName(sec)) +
              " (SEC[1:0]=" + std::to_string(sec) +
              ", OperationStatus=" + hex(opStatus, 8) + ")";
        return true;
    }
};

std::unique_ptr<Driver> makeBq30z55Driver() {
    return std::make_unique<Bq30z55Driver>();
}

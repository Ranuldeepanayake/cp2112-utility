// Implements Smart Battery and diagnostic operations for the TI BQ40Z60 driver.
#include "DeviceDriver.h"
#include <sstream>
#include <vector>

namespace {
// SBS standard word commands
constexpr BYTE CMD_MANUFACTURER_ACCESS        = 0x00;
constexpr BYTE CMD_MANUFACTURER_BLOCK_ACCESS  = 0x44;
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

// Direct block SBS status registers (section 11.42-11.49 of SLUUA04D)
constexpr BYTE CMD_SAFETY_ALERT         = 0x50;
constexpr BYTE CMD_SAFETY_STATUS        = 0x51;
constexpr BYTE CMD_PF_ALERT             = 0x52;
constexpr BYTE CMD_PF_STATUS            = 0x53;
constexpr BYTE CMD_OPERATION_STATUS     = 0x54;
constexpr BYTE CMD_CHARGING_STATUS      = 0x55;
constexpr BYTE CMD_GAUGING_STATUS       = 0x56;
constexpr BYTE CMD_MANUFACTURING_STATUS = 0x57;

// Direct block data registers (section 11.63-11.68 of SLUUA04D)
// These are also readable via MAC subcommands 0x0071-0x0077.
constexpr BYTE CMD_DA_STATUS1    = 0x71; // 32 bytes: voltages, currents, power per cell
constexpr BYTE CMD_DA_STATUS2    = 0x72; // 14 bytes: temperatures
constexpr BYTE CMD_GAUGE_STATUS1 = 0x73; // 32 bytes: TrueRem, Init, Reserve, Ra, CompRes
constexpr BYTE CMD_GAUGE_STATUS2 = 0x74; // 32 bytes: grid, DOD0, StateTime, DODEOC
constexpr BYTE CMD_GAUGE_STATUS3 = 0x75; // 32 bytes: QMax, QMaxDOD0, thermal model
constexpr BYTE CMD_CB_STATUS     = 0x76; // 32 bytes: cell balance times

// MAC subcommands (section 11.1 of SLUUA04D)
constexpr unsigned MAC_SOH          = 0x0077; // 4 bytes: SoH FCC + energy
constexpr unsigned MAC_LED_TOGGLE   = 0x002B;
constexpr unsigned MAC_LED_PRESS    = 0x002C;
constexpr unsigned MAC_SEAL_DEVICE  = 0x0030;
constexpr unsigned MAC_DEVICE_RESET = 0x0041;

// Default factory security keys (SLUUA04D §10.5).
constexpr unsigned DEFAULT_UNSEAL_KEY_1      = 0x0414;
constexpr unsigned DEFAULT_UNSEAL_KEY_2      = 0x3672;
constexpr unsigned DEFAULT_FULL_ACCESS_KEY_1 = 0xFFFF;
constexpr unsigned DEFAULT_FULL_ACCESS_KEY_2 = 0xFFFF;

// OPERATION_STATUS bits [9:8] = SEC[1:0] (BQ40Z60 encoding).
// 01 = Unsealed, 10 = Full Access, 11 = Sealed, 00 = Reserved
static const char* bq40z60SecurityModeName(unsigned sec) {
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

// Reads an N-byte block directly from a BQ40Z60 SBS block register.
bool readBlockN(Bus& bus, const DeviceConfig& d, BYTE cmd,
                size_t n, std::vector<BYTE>& data) {
    std::vector<BYTE> response;
    if (!bus.read(d.address, cmd, 1, static_cast<unsigned>(n + 1), response) ||
        response.size() != n + 1 || response[0] != n) return false;
    data.assign(response.begin() + 1, response.end());
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
        response.size() != dataLength + 1 || response[0] != dataLength) {
        return false;
    }
    data.assign(response.begin() + 1, response.end());
    return true;
}

bool sendMacCommand(Bus& bus, const DeviceConfig& d, unsigned subcommand) {
    return bus.writeBytes(d.address, {
        CMD_MANUFACTURER_ACCESS,
        static_cast<BYTE>(subcommand & 0xff),
        static_cast<BYTE>((subcommand >> 8) & 0xff)
    });
}

unsigned readLEWord(const std::vector<BYTE>& data, size_t offset) {
    return static_cast<unsigned>(data[offset]) |
           (static_cast<unsigned>(data[offset + 1]) << 8);
}

uint32_t readLEWord32(const std::vector<BYTE>& data, size_t offset) {
    return static_cast<uint32_t>(data[offset])            |
           (static_cast<uint32_t>(data[offset + 1]) << 8)  |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

double deciKelvinToCelsius(unsigned value) {
    return value / 10.0 - 273.15;
}
}

struct Bq40z60Driver : Driver {
    std::string title() const override {
        return "Texas Instruments BQ40Z60 battery pack manager";
    }
    std::string capabilities() const override {
        return "Read SBS telemetry, direct safety/protection/operation status flags, "
               "per-cell voltages/currents/power via DAStatus1(0x71), temperatures via "
               "DAStatus2(0x72), Impedance Track gauging state via GaugeStatus1-3(0x73-0x75), "
               "cell balancing times via CBStatus(0x76), State of Health via MAC 0x0077; "
               "LED display test and device reset commands";
    }
    std::vector<DriverRegister> registers() const override {
        return {
            {0x03, "BATTERY_MODE",         "Battery operating and capacity mode flags",   true, false},
            {0x08, "TEMPERATURE",          "Battery temperature (0.1K)",                  true, false},
            {0x09, "VOLTAGE",              "Pack voltage (mV)",                           true, false},
            {0x0A, "CURRENT",              "Instantaneous signed pack current (mA)",      true, false},
            {0x0B, "AVERAGE_CURRENT",      "Average signed pack current (mA)",            true, false},
            {0x0D, "RELATIVE_SOC",         "Relative state of charge (%)",                true, false},
            {0x0F, "REMAINING_CAPACITY",   "Remaining capacity (mAh or 10mWh)",           true, false},
            {0x10, "FULL_CHARGE_CAPACITY", "Full charge capacity (mAh or 10mWh)",         true, false},
            {0x16, "BATTERY_STATUS",       "SBS battery status flags",                    true, false},
            {0x17, "CYCLE_COUNT",          "Charge cycle count",                          true, false},
            {0x23, "MANUFACTURER_DATA",    "MAC subcommand response block",               true, true},
            {0x50, "SAFETY_ALERT",         "Active safety alert flags (32-bit block)",    true, false},
            {0x51, "SAFETY_STATUS",        "Latched safety status flags (32-bit block)",  true, false},
            {0x52, "PF_ALERT",             "Permanent fail alert flags (32-bit block)",   true, false},
            {0x53, "PF_STATUS",            "Permanent fail status flags (32-bit block)",  true, false},
            {0x54, "OPERATION_STATUS",     "Device operation status (32-bit block)",      true, false},
            {0x55, "CHARGING_STATUS",      "Charging algorithm status (32-bit block)",    true, false},
            {0x56, "GAUGING_STATUS",       "Impedance Track gauging status (32-bit)",     true, false},
            {0x57, "MANUFACTURING_STATUS", "Manufacturing and test mode status (32-bit)", true, false},
            {0x71, "DA_STATUS1",           "Per-cell voltages, currents, power (32 B)",   true, false},
            {0x72, "DA_STATUS2",           "Temperature sensors (14 B)",                  true, false},
            {0x73, "GAUGE_STATUS1",        "IT TrueRem, Init, Reserve, Ra, CompRes",      true, false},
            {0x74, "GAUGE_STATUS2",        "IT grid, DOD0, StateTime, DODEOC",            true, false},
            {0x75, "GAUGE_STATUS3",        "IT QMax, QMaxDOD0, QMaxPassedQ, thermal",     true, false},
            {0x76, "CB_STATUS",            "Cell balancing times per cell (32 B)",         true, false},
        };
    }

    bool readResult(Bus& bus, const DeviceConfig& d, DeviceResult& result) override {
        // -- SBS word reads ---------------------------------------------------
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

        const double temperatureC    = deciKelvinToCelsius(temperature);
        const int    currentMa       = static_cast<int16_t>(current);
        const int    avgCurrentMa    = static_cast<int16_t>(averageCurrent);
        const bool   capacityMode    = (batteryMode & 0x8000) != 0;
        const char*  capacityUnit    = capacityMode ? "10mwh" : "mah";

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

        // -- Direct block status reads (0x50-0x57) ----------------------------
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
            std::vector<BYTE> blk;
            if (!readBlockN(bus, d, statusCmds[i], 4, blk)) {
                result.error = std::string("Status register read failed (") +
                               statusKeys[i] + ")";
                return false;
            }
            const unsigned status = static_cast<unsigned>(blk[0])         |
                                    (static_cast<unsigned>(blk[1]) << 8)   |
                                    (static_cast<unsigned>(blk[2]) << 16)  |
                                    (static_cast<unsigned>(blk[3]) << 24);
            result.add(statusKeys[i], static_cast<uint64_t>(status), {}, static_cast<uint64_t>(status));
        }

        // -- DAStatus1 (0x71): 32 bytes ---------------------------------------
        // cell1-4 voltage (mV), BAT/PACK voltage (mV),
        // cell1-4 current (mA signed), cell1-4 power (cW signed),
        // pack power (cW signed), average power (cW signed)
        std::vector<BYTE> da1;
        if (!readBlockN(bus, d, CMD_DA_STATUS1, 32, da1)) {
            result.error = "BQ40Z60 DAStatus1 read failed";
            return false;
        }
        result.add("cell1_voltage_mv",  static_cast<uint64_t>(readLEWord(da1,  0)), "mV");
        result.add("cell2_voltage_mv",  static_cast<uint64_t>(readLEWord(da1,  2)), "mV");
        result.add("cell3_voltage_mv",  static_cast<uint64_t>(readLEWord(da1,  4)), "mV");
        result.add("cell4_voltage_mv",  static_cast<uint64_t>(readLEWord(da1,  6)), "mV");
        result.add("bat_voltage_mv",    static_cast<uint64_t>(readLEWord(da1,  8)), "mV");
        result.add("pack_voltage_mv",   static_cast<uint64_t>(readLEWord(da1, 10)), "mV");
        result.add("cell1_current_ma",  static_cast<int64_t>(static_cast<int16_t>(readLEWord(da1, 12))), "mA");
        result.add("cell2_current_ma",  static_cast<int64_t>(static_cast<int16_t>(readLEWord(da1, 14))), "mA");
        result.add("cell3_current_ma",  static_cast<int64_t>(static_cast<int16_t>(readLEWord(da1, 16))), "mA");
        result.add("cell4_current_ma",  static_cast<int64_t>(static_cast<int16_t>(readLEWord(da1, 18))), "mA");
        result.add("cell1_power_cw",    static_cast<int64_t>(static_cast<int16_t>(readLEWord(da1, 20))), "cW");
        result.add("cell2_power_cw",    static_cast<int64_t>(static_cast<int16_t>(readLEWord(da1, 22))), "cW");
        result.add("cell3_power_cw",    static_cast<int64_t>(static_cast<int16_t>(readLEWord(da1, 24))), "cW");
        result.add("cell4_power_cw",    static_cast<int64_t>(static_cast<int16_t>(readLEWord(da1, 26))), "cW");
        result.add("power_cw",          static_cast<int64_t>(static_cast<int16_t>(readLEWord(da1, 28))), "cW");
        result.add("average_power_cw",  static_cast<int64_t>(static_cast<int16_t>(readLEWord(da1, 30))), "cW");

        // -- DAStatus2 (0x72): 14 bytes ---------------------------------------
        // int, ts1-4, cell, fet temperatures (0.1K each)
        std::vector<BYTE> da2;
        if (!readBlockN(bus, d, CMD_DA_STATUS2, 14, da2)) {
            result.error = "BQ40Z60 DAStatus2 read failed";
            return false;
        }
        result.add("internal_temperature_c", deciKelvinToCelsius(readLEWord(da2,  0)), "C");
        result.add("ts1_temperature_c",      deciKelvinToCelsius(readLEWord(da2,  2)), "C");
        result.add("ts2_temperature_c",      deciKelvinToCelsius(readLEWord(da2,  4)), "C");
        result.add("ts3_temperature_c",      deciKelvinToCelsius(readLEWord(da2,  6)), "C");
        result.add("ts4_temperature_c",      deciKelvinToCelsius(readLEWord(da2,  8)), "C");
        result.add("cell_temperature_c",     deciKelvinToCelsius(readLEWord(da2, 10)), "C");
        result.add("fet_temperature_c",      deciKelvinToCelsius(readLEWord(da2, 12)), "C");

        // -- GaugeStatus1 (0x73): 32 bytes ------------------------------------
        // TrueRemQ (signed mAh), TrueRemE (signed cWh), InitQ, InitE,
        // ReserveQ, ReserveE, T_sim (0.1K), T_ambient (0.1K),
        // RaScale 0-3, CompRes 0-3
        std::vector<BYTE> gs1;
        if (!readBlockN(bus, d, CMD_GAUGE_STATUS1, 32, gs1)) {
            result.error = "BQ40Z60 GaugeStatus1 read failed";
            return false;
        }
        result.add("true_rem_q_mah",    static_cast<int64_t>(static_cast<int16_t>(readLEWord(gs1,  0))), "mAh");
        result.add("true_rem_e_cwh",    static_cast<int64_t>(static_cast<int16_t>(readLEWord(gs1,  2))), "cWh");
        result.add("init_q_mah",        static_cast<uint64_t>(readLEWord(gs1,  4)), "mAh");
        result.add("init_e_cwh",        static_cast<uint64_t>(readLEWord(gs1,  6)), "cWh");
        result.add("reserve_q_mah",     static_cast<uint64_t>(readLEWord(gs1,  8)), "mAh");
        result.add("reserve_e_cwh",     static_cast<uint64_t>(readLEWord(gs1, 10)), "cWh");
        result.add("t_sim_c",           deciKelvinToCelsius(readLEWord(gs1, 12)), "C");
        result.add("t_ambient_c",       deciKelvinToCelsius(readLEWord(gs1, 14)), "C");
        result.add("ra_scale_cell1",    static_cast<uint64_t>(readLEWord(gs1, 16)));
        result.add("ra_scale_cell2",    static_cast<uint64_t>(readLEWord(gs1, 18)));
        result.add("ra_scale_cell3",    static_cast<uint64_t>(readLEWord(gs1, 20)));
        result.add("ra_scale_cell4",    static_cast<uint64_t>(readLEWord(gs1, 22)));
        result.add("comp_res_cell1",    static_cast<uint64_t>(readLEWord(gs1, 24)));
        result.add("comp_res_cell2",    static_cast<uint64_t>(readLEWord(gs1, 26)));
        result.add("comp_res_cell3",    static_cast<uint64_t>(readLEWord(gs1, 28)));
        result.add("comp_res_cell4",    static_cast<uint64_t>(readLEWord(gs1, 30)));

        // -- GaugeStatus2 (0x74): 32 bytes ------------------------------------
        // PackGrid(U1), LStatus(U1), CellGrid1-4(U1 each),
        // StateTime(U4), DOD0_1-4(I2), DOD0PassedQ(I2), DOD0PassedE(I2),
        // DOD0Time(U2), DODEOC1-4(U2)
        std::vector<BYTE> gs2;
        if (!readBlockN(bus, d, CMD_GAUGE_STATUS2, 32, gs2)) {
            result.error = "BQ40Z60 GaugeStatus2 read failed";
            return false;
        }
        result.add("pack_grid",           static_cast<uint64_t>(gs2[0]));
        result.add("learned_status",      static_cast<uint64_t>(gs2[1]), {}, static_cast<uint64_t>(gs2[1]));
        result.add("cell_grid1",          static_cast<uint64_t>(gs2[2]));
        result.add("cell_grid2",          static_cast<uint64_t>(gs2[3]));
        result.add("cell_grid3",          static_cast<uint64_t>(gs2[4]));
        result.add("cell_grid4",          static_cast<uint64_t>(gs2[5]));
        result.add("state_time_s",        static_cast<uint64_t>(readLEWord32(gs2, 6)));
        result.add("dod0_cell1",          static_cast<int64_t>(static_cast<int16_t>(readLEWord(gs2, 10))));
        result.add("dod0_cell2",          static_cast<int64_t>(static_cast<int16_t>(readLEWord(gs2, 12))));
        result.add("dod0_cell3",          static_cast<int64_t>(static_cast<int16_t>(readLEWord(gs2, 14))));
        result.add("dod0_cell4",          static_cast<int64_t>(static_cast<int16_t>(readLEWord(gs2, 16))));
        result.add("dod0_passed_q",       static_cast<int64_t>(static_cast<int16_t>(readLEWord(gs2, 18))));
        result.add("dod0_passed_e",       static_cast<int64_t>(static_cast<int16_t>(readLEWord(gs2, 20))));
        result.add("dod0_time_s",         static_cast<uint64_t>(readLEWord(gs2, 22)));
        result.add("dodeoc_cell1",        static_cast<uint64_t>(readLEWord(gs2, 24)));
        result.add("dodeoc_cell2",        static_cast<uint64_t>(readLEWord(gs2, 26)));
        result.add("dodeoc_cell3",        static_cast<uint64_t>(readLEWord(gs2, 28)));
        result.add("dodeoc_cell4",        static_cast<uint64_t>(readLEWord(gs2, 30)));

        // -- GaugeStatus3 (0x75): 32 bytes ------------------------------------
        // QMax0-3(U2 mAh), QMaxDOD0_0-3(U2), QMaxPassedQ(I2 mAh),
        // QMaxTime(U2 s), TempK(U2), TempA(U2); bytes 24-31 reserved
        std::vector<BYTE> gs3;
        if (!readBlockN(bus, d, CMD_GAUGE_STATUS3, 32, gs3)) {
            result.error = "BQ40Z60 GaugeStatus3 read failed";
            return false;
        }
        result.add("qmax_cell1",          static_cast<uint64_t>(readLEWord(gs3,  0)), "mAh");
        result.add("qmax_cell2",          static_cast<uint64_t>(readLEWord(gs3,  2)), "mAh");
        result.add("qmax_cell3",          static_cast<uint64_t>(readLEWord(gs3,  4)), "mAh");
        result.add("qmax_cell4",          static_cast<uint64_t>(readLEWord(gs3,  6)), "mAh");
        result.add("qmax_dod0_cell1",     static_cast<uint64_t>(readLEWord(gs3,  8)));
        result.add("qmax_dod0_cell2",     static_cast<uint64_t>(readLEWord(gs3, 10)));
        result.add("qmax_dod0_cell3",     static_cast<uint64_t>(readLEWord(gs3, 12)));
        result.add("qmax_dod0_cell4",     static_cast<uint64_t>(readLEWord(gs3, 14)));
        result.add("qmax_passed_q_mah",   static_cast<int64_t>(static_cast<int16_t>(readLEWord(gs3, 16))), "mAh");
        result.add("qmax_time_s",         static_cast<uint64_t>(readLEWord(gs3, 18)), "s");
        result.add("temp_k_factor",       static_cast<uint64_t>(readLEWord(gs3, 20)));
        result.add("temp_a_model",        static_cast<uint64_t>(readLEWord(gs3, 22)));

        // -- CBStatus (0x76): 32 bytes ----------------------------------------
        // CellBalanceTime0-3 (U2 each, seconds); bytes 8-31 reserved
        std::vector<BYTE> cb;
        if (!readBlockN(bus, d, CMD_CB_STATUS, 32, cb)) {
            result.error = "BQ40Z60 CBStatus read failed";
            return false;
        }
        result.add("cb_time_cell1_s", static_cast<uint64_t>(readLEWord(cb, 0)), "s");
        result.add("cb_time_cell2_s", static_cast<uint64_t>(readLEWord(cb, 2)), "s");
        result.add("cb_time_cell3_s", static_cast<uint64_t>(readLEWord(cb, 4)), "s");
        result.add("cb_time_cell4_s", static_cast<uint64_t>(readLEWord(cb, 6)), "s");

        // -- State of Health (MAC 0x0077): 4 bytes ----------------------------
        std::vector<BYTE> soh;
        if (!readMacData(bus, d, MAC_SOH, 4, soh)) {
            result.error = "BQ40Z60 State of Health MAC read failed";
            return false;
        }
        result.add("soh_fcc_mah",    static_cast<uint64_t>(readLEWord(soh, 0)), "mAh");
        result.add("soh_energy_cwh", static_cast<uint64_t>(readLEWord(soh, 2)), "cWh");

        result.succeeded = true;
        return true;
    }

    std::vector<DriverAction> actions() const override {
        const std::string ledWarning =
            "This is a display test command; it does not control charge/discharge FETs.";
        return {
            {"led-toggle",      "Toggle LED display (MAC 0x002B)", {}, ledWarning},
            {"led-press",       "Simulate LED display button press (MAC 0x002C)", {}, ledWarning},
            {"device-reset",    "Send Device Reset (MAC 0x0041).", {},
             "Resets the BQ40Z60 firmware. The device will restart and briefly drop off the bus."},
            {"security-status", "Read current security mode from OperationStatus bits [9:8]", {}, {}},
            {"seal",            "Seal the device (MAC 0x0030). Requires UNSEALED or FULL ACCESS mode.", {}, {}},
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
            {"write-mac",
             "Write data to a MAC subcommand via ManufacturerBlockAccess (0x44). "
             "Requires UNSEALED or FULL ACCESS mode. "
             "Pass the subcommand followed by one or more data bytes, all in hex. "
             "Example: write-mac 0x4605 0x00 0x00",
             {"subcommand (hex)", "data byte(s) (hex, 1–30 bytes)"},
             "This writes directly to device data flash. An incorrect subcommand address "
             "or wrong data value can corrupt gauging calibration. Verify the subcommand "
             "address and expected data format from the device TRM before proceeding."},
        };
    }

    bool executeAction(Bus& bus, const DeviceConfig& d, const std::string& action,
                       const std::vector<std::string>& parameters, std::string& result) override {
        if (action == "led-toggle" || action == "led-press" || action == "device-reset") {
            if (!parameters.empty()) {
                result = "BQ40Z60 " + action + " takes no parameters";
                return false;
            }
            unsigned command;
            if      (action == "led-toggle")   command = MAC_LED_TOGGLE;
            else if (action == "led-press")    command = MAC_LED_PRESS;
            else                               command = MAC_DEVICE_RESET;
            if (!sendMacCommand(bus, d, command)) {
                result = "BQ40Z60 command failed: " + hex(command, 4);
                return false;
            }
            result = "BQ40Z60 command sent: " + hex(command, 4);
            return true;
        }

        if (action == "security-status") {
            if (!parameters.empty()) { result = "BQ40Z60 security-status takes no parameters"; return false; }
            return readBq40z60SecurityStatus(bus, d, result);
        }

        if (action == "seal") {
            if (!parameters.empty()) { result = "BQ40Z60 seal takes no parameters"; return false; }
            if (!sendMacCommand(bus, d, MAC_SEAL_DEVICE)) {
                result = "BQ40Z60 seal command failed";
                return false;
            }
            std::string statusStr;
            readBq40z60SecurityStatus(bus, d, statusStr);
            result = "BQ40Z60 seal command sent. " + statusStr;
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
            if (!bus.writeBytes(d.address, {CMD_MANUFACTURER_ACCESS,
                    static_cast<BYTE>(word1 & 0xFF), static_cast<BYTE>((word1 >> 8) & 0xFF)}) ||
                !bus.writeBytes(d.address, {CMD_MANUFACTURER_ACCESS,
                    static_cast<BYTE>(word2 & 0xFF), static_cast<BYTE>((word2 >> 8) & 0xFF)})) {
                result = "BQ40Z60 key write failed (key: " + hex(word1, 4) + " / " + hex(word2, 4) + ")";
                return false;
            }
            std::string statusStr;
            readBq40z60SecurityStatus(bus, d, statusStr);
            result = "BQ40Z60 key sequence sent (" + hex(word1, 4) + " / " + hex(word2, 4) + "). " + statusStr;
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

        if (action == "write-mac") {
            if (parameters.size() < 2) {
                result = "write-mac requires a subcommand and at least one data byte";
                return false;
            }
            unsigned subcommand;
            try { subcommand = static_cast<unsigned>(std::stoul(parameters[0], nullptr, 0)); }
            catch (...) { result = "Invalid subcommand — expected hex e.g. 0x4605"; return false; }
            if (subcommand > 0xFFFF) { result = "Subcommand out of range (0x0000–0xFFFF)"; return false; }
            std::vector<BYTE> data;
            std::istringstream byteStream(parameters[1]);
            std::string token;
            while (byteStream >> token) {
                unsigned byteVal;
                try { byteVal = static_cast<unsigned>(std::stoul(token, nullptr, 0)); }
                catch (...) { result = "Invalid data byte at index " + std::to_string(data.size()) + ": " + token; return false; }
                if (byteVal > 0xFF) { result = "Data byte at index " + std::to_string(data.size()) + " out of range (0x00–0xFF)"; return false; }
                data.push_back(static_cast<BYTE>(byteVal));
            }
            if (data.empty()) { result = "write-mac requires at least one data byte"; return false; }
            if (data.size() > 30) { result = "Maximum 30 data bytes per write-mac (SMBus ByteCount limit)"; return false; }
            std::vector<BYTE> packet = {
                CMD_MANUFACTURER_BLOCK_ACCESS,
                static_cast<BYTE>(2 + data.size()),
                static_cast<BYTE>(subcommand & 0xFF),
                static_cast<BYTE>((subcommand >> 8) & 0xFF)
            };
            packet.insert(packet.end(), data.begin(), data.end());
            if (!bus.writeBytes(d.address, packet)) {
                result = "write-mac failed (subcommand=" + hex(subcommand, 4) +
                         ") — verify security mode and subcommand address";
                return false;
            }
            std::string bytes;
            for (size_t i = 0; i < data.size(); ++i) {
                if (i > 0) bytes += " ";
                bytes += hex(static_cast<unsigned>(data[i]), 2);
            }
            result = "write-mac sent: subcommand=" + hex(subcommand, 4) + " data=[" + bytes + "]"
                     " — allow 50–100 ms for NVM programming before reading back";
            return true;
        }

        result = "Unknown BQ40Z60 action: " + action;
        return false;
    }

private:
    static bool readBq40z60SecurityStatus(Bus& bus, const DeviceConfig& d, std::string& out) {
        std::vector<BYTE> blk;
        if (!readBlockN(bus, d, CMD_OPERATION_STATUS, 4, blk)) {
            out = "Security status: unreadable (OperationStatus read failed)";
            return false;
        }
        const unsigned opStatus = static_cast<unsigned>(blk[0])        |
                                  (static_cast<unsigned>(blk[1]) << 8)  |
                                  (static_cast<unsigned>(blk[2]) << 16) |
                                  (static_cast<unsigned>(blk[3]) << 24);
        const unsigned sec = (opStatus >> 8) & 0x3;
        out = "Security mode: " + std::string(bq40z60SecurityModeName(sec)) +
              " (SEC[1:0]=" + std::to_string(sec) +
              ", OperationStatus=" + hex(opStatus, 8) + ")";
        return true;
    }
};

std::unique_ptr<Driver> makeBq40z60Driver() {
    return std::make_unique<Bq40z60Driver>();
}

// Defines the common interface implemented by each compiled-in device driver.
#pragma once
#include "AppTypes.h"
#include "CP2112Bus.h"
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

struct DriverAction {
    std::string id;
    std::string label;
    std::vector<std::string> parameters;
    std::string confirmation;
};

struct DriverRegister {
    unsigned address = 0;
    std::string name;
    std::string purpose;
    bool readable = true;
    bool writable = false;
};

// A typed scalar value keeps the device model useful to GUI and API clients
// without requiring them to parse the CLI's formatted display text.
using DeviceValue = std::variant<std::monostate, int64_t, uint64_t, double, bool, std::string>;

struct DeviceReading {
    std::string key;
    DeviceValue raw;
    DeviceValue value;
    std::string unit;
};

struct DeviceResult {
    bool succeeded = false;
    std::string error;
    std::vector<DeviceReading> readings;

    void add(const std::string& key, DeviceValue value, const std::string& unit = {},
             DeviceValue raw = {}) {
        readings.push_back({key, std::move(raw), std::move(value), unit});
    }
};

inline std::string deviceValueText(const DeviceValue& value) {
    return std::visit([](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) return {};
        else if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>) return item;
        else { std::ostringstream text; text << item; return text.str(); }
    }, value);
}

// The CLI renderer is only one presentation of the structured result.
inline std::string formatDeviceResult(const DeviceResult& result) {
    if (!result.succeeded) return "ERROR: " + result.error;
    std::ostringstream output;
    for (size_t i = 0; i < result.readings.size(); ++i) {
        if (i) output << ", ";
        const DeviceReading& reading = result.readings[i];
        output << reading.key << "=" << deviceValueText(reading.value);
        if (!reading.unit.empty()) output << " " << reading.unit;
        if (!std::holds_alternative<std::monostate>(reading.raw)) {
            output << " (raw=" << deviceValueText(reading.raw) << ")";
        }
    }
    return output.str();
}

struct Driver {
    virtual ~Driver() = default;
    virtual std::string title() const = 0;
    virtual bool readResult(Bus& bus, const DeviceConfig& d, DeviceResult& result) = 0;
    virtual std::string readStream(Bus& bus, const DeviceConfig& d) {
        DeviceResult result;
        readResult(bus, d, result);
        return formatDeviceResult(result);
    }
    virtual std::string capabilities() const = 0;
    // Clears driver-side initialization state after a device disappears and
    // is later reconnected during a long-running stream.
    virtual void resetState() {}
    virtual std::vector<DriverAction> actions() const = 0;
    virtual std::vector<DriverRegister> registers() const { return {}; }
    virtual bool executeAction(Bus& bus, const DeviceConfig& d,
                               const std::string& action,
                               const std::vector<std::string>& parameters,
                               std::string& result) = 0;
};

inline int16_t signed16(BYTE hi, BYTE lo) {
    return static_cast<int16_t>((static_cast<unsigned>(hi) << 8) | lo);
}
inline bool parseDriverNumber(const std::string& s, unsigned long& out) {
    try { size_t n = 0; out = std::stoul(s, &n, 0); return n == s.size(); }
    catch (...) { return false; }
}

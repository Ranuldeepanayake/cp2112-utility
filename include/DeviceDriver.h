// Defines the common interface implemented by each compiled-in device driver.
#pragma once
#include "AppTypes.h"
#include "CP2112Bus.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct DriverAction {
    std::string id;
    std::string label;
    std::vector<std::string> parameters;
    std::string confirmation;
};

struct Driver {
    virtual ~Driver() = default;
    virtual std::string title() const = 0;
    virtual std::string readStream(Bus& bus, const DeviceConfig& d) = 0;
    virtual std::string capabilities() const = 0;
    virtual std::vector<DriverAction> actions() const = 0;
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

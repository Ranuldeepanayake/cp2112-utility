// Implements the page-aware 24C32BN I2C EEPROM driver.
#include "DeviceDriver.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace {
constexpr unsigned MemorySize = 4096;
constexpr unsigned PageSize = 32;
constexpr unsigned MaxRead = 512;
constexpr unsigned MaxResponseReport = 61;

bool parseValue(const std::string& text, unsigned maximum, unsigned& value) {
    unsigned long parsed = 0;
    if (!parseDriverNumber(text, parsed) || parsed > maximum) return false;
    value = static_cast<unsigned>(parsed);
    return true;
}

bool parseByteStream(const std::string& text, std::vector<BYTE>& bytes) {
    bytes.clear();
    std::string normalized = text;
    for (char& character : normalized)
        if (character == ',' || character == ';') character = ' ';
    std::istringstream input(normalized);
    std::string token;
    while (input >> token) {
        unsigned value = 0;
        if (!parseValue(token, 0xFF, value)) return false;
        bytes.push_back(static_cast<BYTE>(value));
    }
    return !bytes.empty();
}

bool readMemory(Bus& bus, const DeviceConfig& device, unsigned address,
                unsigned length, std::vector<BYTE>& bytes, std::string& error) {
    if (address >= MemorySize || length == 0 || length > MaxRead || address + length > MemorySize) {
        error = "EEPROM read range must be within 0x000..0xFFF and contain 1..512 bytes";
        return false;
    }
    bytes.clear();
    while (bytes.size() < length) {
        const unsigned offset = static_cast<unsigned>(bytes.size());
        const unsigned chunkLength = std::min(MaxResponseReport, length - offset);
        std::vector<BYTE> chunk;
        if (!bus.read(device.address, address + offset, 2, chunkLength, chunk) ||
            chunk.size() != chunkLength) {
            // Some CP2112/EEPROM combinations reject a multi-byte address
            // read even though single-byte random reads work. Retry the
            // remainder byte-by-byte without changing the public action.
            if (chunkLength > 1) {
                for (unsigned byteOffset = offset; byteOffset < length; ++byteOffset) {
                    std::vector<BYTE> single;
                    if (!bus.read(device.address, address + byteOffset, 2, 1, single) || single.size() != 1) {
                        error = "24C32BN EEPROM read failed at memory address 0x";
                        std::ostringstream addressText;
                        addressText << std::hex << (address + byteOffset);
                        error += addressText.str();
                        return false;
                    }
                    bytes.push_back(single.front());
                }
                return bytes.size() == length;
            }
            error = "24C32BN EEPROM read failed at memory address 0x";
            std::ostringstream addressText;
            addressText << std::hex << (address + offset);
            error += addressText.str();
            return false;
        }
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    }
    return true;
}

bool writeMemory(Bus& bus, const DeviceConfig& device, unsigned address,
                 const std::vector<BYTE>& bytes, std::string& error) {
    if (address >= MemorySize || bytes.empty() || address + bytes.size() > MemorySize) {
        error = "EEPROM write range must be within 0x000..0xFFF";
        return false;
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        const unsigned currentAddress = address + static_cast<unsigned>(offset);
        const unsigned pageRemaining = PageSize - (currentAddress % PageSize);
        const size_t chunkLength = std::min<size_t>(pageRemaining, bytes.size() - offset);
        std::vector<BYTE> request = {
            static_cast<BYTE>((currentAddress >> 8) & 0xFF),
            static_cast<BYTE>(currentAddress & 0xFF)
        };
        request.insert(request.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + chunkLength));
        if (!bus.writeBytes(device.address, request)) {
            error = "24C32BN EEPROM page write failed";
            return false;
        }
        // Wait for the EEPROM's internal write cycle before the next page.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        offset += chunkLength;
    }
    return true;
}

std::string bytesText(const std::vector<BYTE>& bytes) {
    std::ostringstream output;
    output << std::hex;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) output << ' ';
        output << "0x" << static_cast<unsigned>(bytes[i]);
    }
    return output.str();
}
}

struct Eeprom24c32bnDriver final : Driver {
    std::string title() const override { return "24C32BN I2C EEPROM"; }
    std::string capabilities() const override {
        return "Read/write individual bytes and bounded byte streams; page-aware writes; static capability information. The 24C32BN has no software reset or identification register.";
    }

    std::vector<DriverRegister> registers() const override {
        return {{0x000, "MEMORY", "Start of the 4096-byte EEPROM address space; use 16-bit memory addresses", true, true},
                {0xFFF, "MEMORY_END", "End of the 4096-byte EEPROM address space", true, true}};
    }

    bool readResult(Bus&, const DeviceConfig& d, DeviceResult& result) override {
        result.succeeded = true;
        result.add("memory_size_bytes", static_cast<uint64_t>(MemorySize), "bytes");
        result.add("page_size_bytes", static_cast<uint64_t>(PageSize), "bytes");
        result.add("memory_address_bytes", static_cast<uint64_t>(2), "bytes");
        result.add("i2c_address", static_cast<uint64_t>(d.address), "7-bit");
        result.add("memory_range", std::string("0x000..0xFFF"));
        result.add("software_reset", std::string("not supported"));
        result.add("identification_register", std::string("not supported"));
        return true;
    }

    std::vector<DriverAction> actions() const override {
        return {
            {"read-info", "Read EEPROM capability information", {}, "Returns static 24C32BN capabilities; the EEPROM has no ID register."},
            {"read-byte", "Read one EEPROM byte", {"Memory address (0x000..0xFFF)"}, ""},
            {"read-stream", "Read EEPROM byte stream", {"Start address (0x000..0xFFF)", "Byte count (1..512)"}, ""},
            {"write-byte", "Write one EEPROM byte", {"Memory address (0x000..0xFFF)", "Byte value (0x00..0xFF)"}, "This changes non-volatile EEPROM data."},
            {"write-stream", "Write EEPROM byte stream", {"Start address (0x000..0xFFF)", "Space-separated bytes (for example 0x01 0x02 0xA5)"}, "This changes non-volatile EEPROM data and is split at 32-byte page boundaries."},
            {"verify-stream", "Verify EEPROM byte stream", {"Start address (0x000..0xFFF)", "Expected space-separated bytes"}, "Reads and compares EEPROM data without changing it."}
        };
    }

    bool executeAction(Bus& bus, const DeviceConfig& d, const std::string& action,
                       const std::vector<std::string>& parameters, std::string& result) override {
        if (action == "read-info") {
            result = "24C32BN: 4096 bytes, 32-byte pages, 16-bit memory address, no ID/reset command";
            return true;
        }
        if (action == "read-byte" || action == "read-stream") {
            if ((action == "read-byte" && parameters.size() != 1) ||
                (action == "read-stream" && parameters.size() != 2)) {
                result = "Invalid read parameter count";
                return false;
            }
            unsigned address = 0, length = 1;
            if (!parseValue(parameters[0], MemorySize - 1, address) ||
                (action == "read-stream" && !parseValue(parameters[1], MaxRead, length))) {
                result = "Invalid EEPROM read address or length";
                return false;
            }
            std::vector<BYTE> bytes;
            if (!readMemory(bus, d, address, length, bytes, result)) return false;
            result = "Read " + std::to_string(bytes.size()) + " byte(s) at " +
                     std::to_string(address) + ": " + bytesText(bytes);
            return true;
        }
        if (action == "write-byte" || action == "write-stream") {
            if (parameters.size() != 2) { result = "Invalid write parameter count"; return false; }
            unsigned address = 0;
            if (!parseValue(parameters[0], MemorySize - 1, address)) {
                result = "Invalid EEPROM write address";
                return false;
            }
            std::vector<BYTE> bytes;
            if (action == "write-byte") {
                unsigned value = 0;
                if (!parseValue(parameters[1], 0xFF, value)) { result = "Invalid EEPROM byte value"; return false; }
                bytes.push_back(static_cast<BYTE>(value));
            } else if (!parseByteStream(parameters[1], bytes)) {
                result = "Enter one or more hexadecimal byte values";
                return false;
            }
            if (!writeMemory(bus, d, address, bytes, result)) return false;
            result = "Wrote " + std::to_string(bytes.size()) + " byte(s) at memory address " + std::to_string(address);
            return true;
        }
        if (action == "verify-stream") {
            if (parameters.size() != 2) { result = "Invalid verify parameter count"; return false; }
            unsigned address = 0;
            std::vector<BYTE> expected;
            if (!parseValue(parameters[0], MemorySize - 1, address) || !parseByteStream(parameters[1], expected)) {
                result = "Invalid EEPROM verify address or byte stream";
                return false;
            }
            std::vector<BYTE> actual;
            if (!readMemory(bus, d, address, static_cast<unsigned>(expected.size()), actual, result)) return false;
            result = actual == expected ? "EEPROM verification succeeded" :
                "EEPROM verification failed; actual bytes: " + bytesText(actual);
            return actual == expected;
        }
        result = "Unknown 24C32BN action";
        return false;
    }
};

std::unique_ptr<Driver> make24c32bnDriver() { return std::make_unique<Eeprom24c32bnDriver>(); }

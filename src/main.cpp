// Provides the interactive console UI for the CP2112 application service.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Application.h"
#include <algorithm>
#include <conio.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <limits>
#include <process.h>
#include <string>
#include <thread>
#include <vector>

namespace {
struct StreamStopState {
    std::atomic<bool> stop{false};
    std::atomic<unsigned> references{2};
};

// Releases the shared stream-stop state after one owner is finished with it.
void releaseStreamStopState(StreamStopState* state) {
    if (state->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete state;
}

// Waits for ENTER on a worker thread so streaming can continue independently.
unsigned __stdcall waitForStreamStop(void* parameter) {
    auto* state = static_cast<StreamStopState*>(parameter);
    std::string line;
    std::getline(std::cin, line);
    state->stop.store(true);
    releaseStreamStopState(state);
    return 0;
}

// Parses decimal or 0x-prefixed numeric input without accepting trailing text.
bool parseUnsigned(const std::string& text, unsigned long& value) {
    try {
        size_t consumed = 0;
        value = std::stoul(text, &consumed, 0);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

// Reads an optional numeric menu value and applies the supplied default on ENTER.
bool promptUnsigned(const std::string& prompt, unsigned defaultValue, unsigned& value) {
    std::cout << prompt;
    std::string input;
    if (!std::getline(std::cin, input)) return false;
    if (input.empty()) {
        value = defaultValue;
        return true;
    }
    unsigned long parsed = 0;
    if (!parseUnsigned(input, parsed) || parsed > std::numeric_limits<unsigned>::max()) return false;
    value = static_cast<unsigned>(parsed);
    return true;
}

// Reads a numeric setting; B or Q cancels without treating zero as cancellation.
bool promptUnsignedCancelable(const std::string& prompt, unsigned defaultValue,
                              unsigned& value, bool& cancelled) {
    std::cout << prompt;
    std::string input;
    if (!std::getline(std::cin, input)) return false;
    if (input == "b" || input == "B" || input == "q" || input == "Q") {
        cancelled = true;
        return true;
    }
    if (input.empty()) {
        value = defaultValue;
        return true;
    }
    unsigned long parsed = 0;
    if (!parseUnsigned(input, parsed) || parsed > std::numeric_limits<unsigned>::max()) return false;
    value = static_cast<unsigned>(parsed);
    return true;
}

// Reads a boolean setting; B or Q cancels the edit.
bool promptBooleanCancelable(const std::string& prompt, bool defaultValue,
                             bool& value, bool& cancelled) {
    std::cout << prompt;
    std::string input;
    if (!std::getline(std::cin, input)) return false;
    if (input == "b" || input == "B" || input == "q" || input == "Q") {
        cancelled = true;
        return true;
    }
    if (input.empty()) {
        value = defaultValue;
        return true;
    }
    std::transform(input.begin(), input.end(), input.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (input == "true" || input == "yes" || input == "1" || input == "on") { value = true; return true; }
    if (input == "false" || input == "no" || input == "0" || input == "off") { value = false; return true; }
    return false;
}

// Tests whether a path points to a regular file rather than a directory.
bool fileExists(const std::string& path) {
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Returns the directory containing the running executable.
std::string executableDirectory() {
    char buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    const std::string executablePath(buffer, length);
    const size_t separator = executablePath.find_last_of("\\/");
    return separator == std::string::npos ? std::string{} : executablePath.substr(0, separator);
}

// Finds the default config file when the program is launched from the root or bin directory.
std::string defaultConfigPath() {
    const std::string executableDir = executableDirectory();
    const std::vector<std::string> candidates = {
        "config\\devices.ini",
        "..\\config\\devices.ini",
        executableDir + "\\config\\devices.ini",
        executableDir + "\\..\\config\\devices.ini"
    };
    for (const std::string& candidate : candidates) {
        if (!candidate.empty() && fileExists(candidate)) return candidate;
    }
    return "config\\devices.ini";
}

// Formats the embedded CI metadata in the user-facing build label.
std::string versionString(const AppConfig& config) {
    std::string version = "Build " + config.buildTimestamp + "-" + config.buildNumber;
    if (config.betaBuild) version += "-beta";
    return version;
}

// Clears the remainder of the current console input line after formatted input.
void discardInputLine() {
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

// Displays CP2112 adapter information obtained through the application service.
void showAdapters(ApplicationService& app) {
    std::vector<AdapterInformation> adapters;
    std::string error;
    if (!app.adapters(adapters, error)) {
        std::cout << "Adapter enumeration failed: " << error << "\n";
        return;
    }
    std::cout << "CP2112 adapters found: " << adapters.size() << "\n";
    for (const auto& adapter : adapters) {
        std::cout << "\n[" << adapter.index << "] " << adapter.product << '\n'
                  << "    Manufacturer: " << adapter.manufacturer << '\n'
                  << "    Serial:       " << adapter.serial << '\n';
        if (adapter.attributesAvailable) {
            std::cout << "    VID:PID:      " << hex(adapter.vid, 4) << ':' << hex(adapter.pid, 4) << '\n'
                      << "    USB release:  " << hex(adapter.release, 4) << '\n';
        } else {
            std::cout << "    Attributes unavailable: " << adapter.attributeStatus << '\n';
        }
        if (adapter.partNumberAvailable) {
            std::cout << "    Part number:   " << hex(adapter.partNumber, 2)
                      << " (version " << static_cast<unsigned>(adapter.partVersion) << ")\n";
        } else {
            std::cout << "    Part number unavailable: " << adapter.partNumberStatus << '\n';
        }
        if (adapter.usbConfigAvailable) {
            std::cout << "    Stored USB cfg: VID:PID=" << hex(adapter.configuredVid, 4)
                      << ':' << hex(adapter.configuredPid, 4)
                      << ", release=" << hex(adapter.configuredRelease, 4)
                      << ", power=" << static_cast<unsigned>(adapter.configuredPower)
                      << ", power mode=" << hex(adapter.configuredPowerMode, 2) << '\n';
        } else {
            std::cout << "    Stored USB config unavailable: " << adapter.usbConfigStatus << '\n';
        }
        if (adapter.lockAvailable) {
            std::cout << "    Config lock bits: " << hex(adapter.lockBits, 2) << '\n';
        } else {
            std::cout << "    Config lock unavailable: " << adapter.lockStatus << '\n';
        }
        if (adapter.smbusConfigAvailable) {
            std::cout << "    SMBus config:   " << adapter.smbusBitRate << " Hz"
                      << ", target=" << hex(adapter.smbusAddress, 2)
                      << ", write timeout=" << adapter.smbusWriteTimeout << " ms"
                      << ", read timeout=" << adapter.smbusReadTimeout << " ms"
                      << ", retries=" << adapter.smbusTransferRetries
                      << ", auto response=" << (adapter.smbusAutoReadRespond ? "on" : "off")
                      << ", SCL-low timeout=" << (adapter.smbusSclLowTimeout ? "on" : "off") << '\n';
        } else {
            std::cout << "    SMBus config unavailable: " << adapter.smbusConfigStatus << '\n';
        }
        if (adapter.smbusLibraryVersionAvailable) {
            std::cout << "    SMBus library: " << static_cast<unsigned>(adapter.smbusLibraryMajor)
                      << '.' << static_cast<unsigned>(adapter.smbusLibraryMinor)
                      << (adapter.smbusLibraryRelease ? " (release)" : " (debug)") << '\n';
        } else {
            std::cout << "    SMBus library unavailable: " << adapter.smbusLibraryVersionStatus << '\n';
        }
        if (adapter.hidLibraryVersionAvailable) {
            std::cout << "    HID library:   " << static_cast<unsigned>(adapter.hidLibraryMajor)
                      << '.' << static_cast<unsigned>(adapter.hidLibraryMinor)
                      << (adapter.hidLibraryRelease ? " (release)" : " (debug)") << '\n';
        } else {
            std::cout << "    HID library unavailable: " << adapter.hidLibraryVersionStatus << '\n';
        }
        std::cout << "    Device path:  " << adapter.devicePath << '\n';
    }
}

// Starts a device stream and owns the ENTER listener used to stop it.
void streamDevice(ApplicationService& app, size_t index) {
    const DeviceConfig device = app.config().devices[index];
    std::cout << "Streaming " << device.name << " every " << device.refreshSeconds
              << " second(s). Press ENTER to stop.\n";
    auto* state = new StreamStopState;
    const uintptr_t listener = _beginthreadex(nullptr, 0, waitForStreamStop, state, 0, nullptr);
    if (listener == 0) {
        delete state;
        std::cout << "Could not start stream stop listener; errno=" << errno << "\n";
        return;
    }
    CloseHandle(reinterpret_cast<HANDLE>(listener));
    std::string error;
    const bool ok = app.streamDevice(index, state->stop,
        [&device](const std::string& timestamp, const std::string& value) {
            std::cout << "[" << timestamp << "] " << device.name << ": " << value << "\n";
        }, error);
    releaseStreamStopState(state);
    if (!ok) std::cout << "Stream failed: " << error << "\n";
}

// Presents and executes the actions exposed by the selected device driver.
void runDriverActions(ApplicationService& app, size_t index) {
    std::string error;
    DriverInformation information;
    if (!app.driverInformation(index, information, error)) {
        std::cout << error << "\n";
        return;
    }
    const std::vector<DriverAction>& actions = information.actions;
    while (true) {
        std::cout << "\nDevice functions:\n";
        for (size_t i = 0; i < actions.size(); ++i) {
            std::cout << i + 1 << ". " << actions[i].label << "\n";
        }
        std::cout << "0 Back\n> ";
        size_t selected = 0;
        if (!(std::cin >> selected)) {
            std::cin.clear();
            discardInputLine();
            return;
        }
        discardInputLine();
        if (selected == 0) return;
        if (selected > actions.size()) {
            std::cout << "Invalid choice.\n";
            continue;
        }

        const DriverAction& action = actions[selected - 1];
        std::vector<std::string> parameters;
        bool inputValid = true;
        for (const std::string& prompt : action.parameters) {
            std::cout << prompt << ": ";
            std::string value;
            if (!std::getline(std::cin, value)) {
                inputValid = false;
                break;
            }
            parameters.push_back(value);
        }
        if (!inputValid) {
            std::cout << "Could not read action parameters.\n";
            return;
        }
        if (!action.confirmation.empty()) {
            std::cout << action.confirmation << " Continue? [y/N]: ";
            std::string confirmation;
            if (!std::getline(std::cin, confirmation) ||
                (confirmation != "y" && confirmation != "Y")) {
                std::cout << "Action cancelled.\n";
                continue;
            }
        }
        std::string result;
        error.clear();
        const bool succeeded = app.executeDeviceAction(index, action.id, parameters, result, error);
        if (!error.empty()) std::cout << error << "\n";
        if (!result.empty()) std::cout << (succeeded ? "OK: " : "Action failed: ") << result << "\n";
    }
}

// Runs the interactive menu for one configured device.
void deviceMenu(ApplicationService& app, size_t index) {
    while (true) {
        const DeviceConfig device = app.config().devices[index];
        DriverInformation information;
        std::string driverError;
        if (!app.driverInformation(index, information, driverError)) {
            std::cout << driverError << "\n";
            return;
        }
        std::cout << "\n=== " << device.name << " (" << information.title << ") ===\n"
                  << "Address: " << hex(device.address) << " | Clock: " << device.clock
                  << " Hz | Refresh: " << device.refreshSeconds << " s | Stream logging: "
                  << (device.logStream ? "on" : "off") << "\n"
                  << "Capabilities: " << information.capabilities << "\n"
                  << "1 Read once\n2 Start data stream\n3 Device configuration/functions\n"
                  << "4 Change refresh interval\n5 Toggle stream logging\n0 Back to device list\n> ";
        int choice;
        if (!(std::cin >> choice)) {
            std::cin.clear();
            discardInputLine();
            continue;
        }
        if (choice == 0) return;
        if (choice == 1) {
            std::string result, error;
            if (app.readDevice(index, result, error)) std::cout << device.name << ": " << result << "\n";
            else std::cout << "Read failed: " << error << "\n";
        } else if (choice == 2) {
            discardInputLine();
            streamDevice(app, index);
        } else if (choice == 3) {
            discardInputLine();
            runDriverActions(app, index);
        } else if (choice == 4) {
            discardInputLine();
            unsigned interval = device.refreshSeconds;
            if (!promptUnsigned("Refresh interval in seconds (1..3600): ", interval, interval)) {
                std::cout << "Invalid interval.\n";
                continue;
            }
            std::string error;
            if (!app.updateRuntimeDevice(index, interval, device.logStream, error)) std::cout << error << "\n";
            else std::cout << "Refresh interval updated for this run.\n";
        } else if (choice == 5) {
            std::string error;
            if (!app.updateRuntimeDevice(index, device.refreshSeconds, !device.logStream, error)) std::cout << error << "\n";
            else std::cout << "Stream logging " << (device.logStream ? "enabled" : "disabled") << " for this run.\n";
        } else {
            std::cout << "Invalid choice.\n";
        }
    }
}

// Displays the enabled device entries using their configuration order.
void showDevices(const AppConfig& config, const std::vector<size_t>& indices) {
    std::cout << "\nConfigured devices:\n";
    for (size_t i = 0; i < indices.size(); ++i) {
        const DeviceConfig& device = config.devices[indices[i]];
        std::cout << i + 1 << ". " << device.name << " [" << device.type << "] address="
                  << hex(device.address) << " clock=" << device.clock << "Hz refresh="
                  << device.refreshSeconds << "s logging=" << (device.logStream ? "on" : "off") << "\n";
    }
}

// Lets the user choose an enabled device before entering its device menu.
void selectDevice(ApplicationService& app) {
    const AppConfig config = app.config();
    std::vector<size_t> indices;
    for (size_t i = 0; i < config.devices.size(); ++i) {
        if (config.devices[i].enabled) indices.push_back(i);
    }
    if (indices.empty()) {
        std::cout << "No enabled devices configured.\n";
        return;
    }
    while (true) {
        showDevices(config, indices);
        std::cout << "Select device number (0 to return to main menu): ";
        size_t selection = 0;
        if (!(std::cin >> selection)) {
            std::cin.clear();
            discardInputLine();
            std::cout << "Invalid selection.\n";
            continue;
        }
        if (selection == 0) return;
        if (selection > indices.size()) {
            std::cout << "Invalid selection.\n";
            continue;
        }
        deviceMenu(app, indices[selection - 1]);
    }
}

// Collects scan settings, reports address progress, and displays scan results.
void scanBus(ApplicationService& app) {
    const AppConfig config = app.config();
    ScanOptions options;
    options.adapterIndex = config.scanAdapterIndex;
    options.startAddress = config.scanStartAddress;
    options.endAddress = config.scanEndAddress;
    options.clockHz = config.scanClockHz;
    options.transferTimeoutMs = config.transferTimeoutMs;
    options.transferRetries = config.transferRetries;
    discardInputLine();
    if (!promptUnsigned("Adapter index [" + std::to_string(options.adapterIndex) + "]: ",
                        options.adapterIndex, options.adapterIndex) ||
        !promptUnsigned("First 7-bit address [" + hex(options.startAddress) + "]: ",
                        options.startAddress, options.startAddress) ||
        !promptUnsigned("Last 7-bit address [" + hex(options.endAddress) + "]: ",
                        options.endAddress, options.endAddress) ||
        !promptUnsigned("Bus speed Hz [" + std::to_string(options.clockHz) + "]: ",
                        options.clockHz, options.clockHz) ||
        !promptUnsigned("Transfer timeout ms [" + std::to_string(options.transferTimeoutMs) + "]: ",
                        options.transferTimeoutMs, options.transferTimeoutMs) ||
        !promptUnsigned("Transfer retries [" + std::to_string(options.transferRetries) + "]: ",
                        options.transferRetries, options.transferRetries)) {
        std::cout << "Invalid numeric scan input.\n";
        return;
    }
    if (options.transferTimeoutMs < 1 || options.transferTimeoutMs > 1000 ||
        options.transferRetries > 1000) {
        std::cout << "Transfer timeout must be 1..1000 ms and retries must be 0..1000.\n";
        return;
    }
    std::vector<ScanReading> readings;
    std::string error;
    std::cout << "Press Q or Esc during scanning to cancel.\n";
    const auto reportProgress = [](unsigned address) {
        std::cout << "Scanning address 7-bit=" << hex(address)
                  << " (" << address << ")"
                  << "...\n" << std::flush;
        return true;
    };
    std::atomic<bool> cancelRequested(false);
    std::atomic<bool> scanFinished(false);
    bool scanSucceeded = false;
    std::thread scanThread([&]() {
        scanSucceeded = app.scan(options, readings, error, reportProgress, &cancelRequested);
        scanFinished.store(true);
    });
    bool cancellationReported = false;
    while (!scanFinished.load()) {
        if (_kbhit()) {
            const int key = _getch();
            if (key == 'q' || key == 'Q' || key == 27) {
                cancelRequested.store(true);
                if (!cancellationReported) {
                    std::cout << "Cancelling active transfer...\n" << std::flush;
                    cancellationReported = true;
                }
            }
        }
        Sleep(10);
    }
    scanThread.join();
    if (!scanSucceeded) {
        std::cout << "Scan failed: " << error << "\n";
        return;
    }
    if (!error.empty()) std::cout << "\n" << error << "\n";
    std::cout << "\nTransfer diagnostics:\n";
    std::cout << "Address  Decimal  Result  API  State  Detail  Retries  Bytes  Timeout  Cancelled\n";
    std::cout << "-------  -------  ------  ---  -----  ------  -------  -----  -------  ---------\n";
    for (const ScanReading& reading : readings) {
        const TransferDiagnostics& transfer = reading.transfer;
        std::cout << std::left << std::setw(9) << hex(reading.address)
                  << std::setw(9) << reading.address
                  << std::setw(8) << (reading.readSucceeded ? "FOUND" : "MISS")
                  << std::setw(5) << transfer.apiStatus
                  << std::setw(7) << transfer.transferState
                  << std::setw(8) << transfer.detailedStatus
                  << std::setw(9) << transfer.retries
                  << std::setw(7) << transfer.bytesTransferred
                  << std::setw(9) << (transfer.timedOut ? "yes" : "no")
                  << (transfer.cancelled ? "yes" : "no") << "\n";
    }
    const size_t hits = static_cast<size_t>(std::count_if(
        readings.begin(), readings.end(), [](const ScanReading& reading) { return reading.readSucceeded; }));
    std::cout << "\nFound devices (read-probe results):\n";
    if (hits == 0) {
        std::cout << "No devices found.\n";
    } else {
        std::cout << "Address (hex)  Address (decimal)  Probe value\n";
        std::cout << "-------------  -----------------  -----------\n";
        for (const ScanReading& reading : readings) {
            if (reading.readSucceeded) {
                std::cout << std::left << std::setw(13) << hex(reading.address)
                          << std::setw(19) << reading.address << hex(reading.value) << "\n";
            }
        }
    }
    std::cout << "Scan complete; successful read probes=" << hits << "\n";
}

// Sends one direct register read or write command selected from the main menu.
void directRegisterCommand(ApplicationService& app) {
    discardInputLine();
    unsigned deviceAddress = 0;
    unsigned registerAddress = 0;
    if (!promptUnsigned("Device I2C address (0x08..0x77): ", 0, deviceAddress) ||
        !promptUnsigned("Register address (0x00..0xFF): ", 0, registerAddress)) {
        std::cout << "Invalid address.\n";
        return;
    }

    std::cout << "Command (r=read, w=write): ";
    std::string command;
    if (!std::getline(std::cin, command) || command.empty()) {
        std::cout << "Invalid command.\n";
        return;
    }
    std::string error;
    if (command == "r" || command == "R") {
        BYTE value = 0;
        if (app.readRegister(deviceAddress, registerAddress, value, error)) {
            std::cout << "Read " << hex(value) << " from device " << hex(deviceAddress)
                      << ", register " << hex(registerAddress) << ".\n";
        } else {
            std::cout << "Direct read failed: " << error << "\n";
        }
    } else if (command == "w" || command == "W") {
        unsigned value = 0;
        if (!promptUnsigned("Byte value (0x00..0xFF): ", 0, value) || value > 0xFF) {
            std::cout << "Invalid byte value.\n";
            return;
        }
        if (app.writeRegister(deviceAddress, registerAddress, static_cast<BYTE>(value), error)) {
            std::cout << "Wrote " << hex(value) << " to device " << hex(deviceAddress)
                      << ", register " << hex(registerAddress) << ".\n";
        } else {
            std::cout << "Direct write failed: " << error << "\n";
        }
    } else {
        std::cout << "Unknown command; use r or w.\n";
    }
}

// Deletes timestamped application logs older than the user-selected age.
void cleanLogs(ApplicationService& app) {
    discardInputLine();
    unsigned days = 0;
    if (!promptUnsigned("Delete logs older than how many days (0 to cancel): ", 0, days)) {
        std::cout << "Invalid number of days.\n";
        return;
    }
    if (days == 0) {
        std::cout << "Log cleanup cancelled.\n";
        return;
    }
    LogCleanupResult result;
    std::string error;
    if (!app.cleanLogs(days, result, error)) {
        std::cout << error << "\n";
        return;
    }
    std::cout << "Deleted " << result.deleted << " old log file(s); failures="
              << result.failed << "\n";
}

// Proof-of-concept frontend for the shared persisted settings API.
void settingsMenu(ApplicationService& app) {
    discardInputLine();
    while (true) {
        const AppConfig config = app.config();
        std::cout << "\nSettings (saved to config/devices.ini):\n"
                  << "1 General settings\n2 Device settings\n0 Back\n> ";
        unsigned category = 0;
        if (!promptUnsigned("", 0, category) || category == 0) return;
        std::string key;
        SettingValue value;
        if (category == 1) {
            std::cout << "\nGeneral settings:\n"
                      << "1 transfer_timeout_ms=" << config.transferTimeoutMs << "\n"
                      << "2 transfer_retries=" << config.transferRetries << "\n"
                      << "3 scan_clock_hz=" << config.scanClockHz << "\n"
                      << "4 scan_start_address=" << hex(config.scanStartAddress) << "\n"
                      << "5 scan_end_address=" << hex(config.scanEndAddress) << "\n"
                      << "6 default_refresh_seconds=" << config.defaultRefreshSeconds << "\n"
                      << "7 default_log_stream=" << (config.defaultLogStream ? "true" : "false") << "\n"
                      << "8 log_retention_days=" << config.logRetentionDays << "\n> ";
            unsigned choice = 0;
            if (!promptUnsigned("General setting (0 to go back): ", 0, choice)) { std::cout << "Invalid setting.\n"; continue; }
            if (choice == 0) continue;
            unsigned number = 0;
            bool boolean = false;
            bool cancelled = false;
            switch (choice) {
            case 1: key = "transfer_timeout_ms"; if (!promptUnsignedCancelable("New timeout [current] (B to go back): ", config.transferTimeoutMs, number, cancelled)) continue; value = static_cast<uint64_t>(number); break;
            case 2: key = "transfer_retries"; if (!promptUnsignedCancelable("New retries [current] (B to go back): ", config.transferRetries, number, cancelled)) continue; value = static_cast<uint64_t>(number); break;
            case 3: key = "scan_clock_hz"; if (!promptUnsignedCancelable("New scan clock [current] (B to go back): ", config.scanClockHz, number, cancelled)) continue; value = static_cast<uint64_t>(number); break;
            case 4: key = "scan_start_address"; if (!promptUnsignedCancelable("New start address [current] (B to go back): ", config.scanStartAddress, number, cancelled)) continue; value = static_cast<uint64_t>(number); break;
            case 5: key = "scan_end_address"; if (!promptUnsignedCancelable("New end address [current] (B to go back): ", config.scanEndAddress, number, cancelled)) continue; value = static_cast<uint64_t>(number); break;
            case 6: key = "default_refresh_seconds"; if (!promptUnsignedCancelable("New default refresh [current] (B to go back): ", config.defaultRefreshSeconds, number, cancelled)) continue; value = static_cast<uint64_t>(number); break;
            case 7: key = "default_log_stream"; if (!promptBooleanCancelable("New default logging (true/false, B to go back) [current]: ", config.defaultLogStream, boolean, cancelled)) continue; value = boolean; break;
            case 8: key = "log_retention_days"; if (!promptUnsignedCancelable("New retention days [current] (B to go back): ", config.logRetentionDays, number, cancelled)) continue; value = static_cast<uint64_t>(number); break;
            default: std::cout << "Invalid setting.\n"; continue;
            }
            if (cancelled) { std::cout << "Setting update cancelled.\n"; continue; }
            std::string error;
            if (!app.updateGeneralSetting(key, value, error)) std::cout << "Setting update failed: " << error << "\n";
            else std::cout << "Saved [general] " << key << ".\n";
        } else if (category == 2) {
            std::cout << "\nDevices:\n";
            for (size_t i = 0; i < config.devices.size(); ++i)
                std::cout << i + 1 << ". " << config.devices[i].name << " [" << config.devices[i].type << "]\n";
            unsigned selected = 0;
            if (!promptUnsigned("Device number (0 to cancel): ", 0, selected) || selected == 0) continue;
            if (selected > config.devices.size()) { std::cout << "Invalid device.\n"; continue; }
            const DeviceConfig& device = config.devices[selected - 1];
            std::cout << "1 address=" << hex(device.address) << "\n2 clock_hz=" << device.clock
                      << "\n3 refresh_seconds=" << device.refreshSeconds
                      << "\n4 adapter_index=" << device.adapterIndex
                      << "\n5 log_stream=" << (device.logStream ? "true" : "false")
                      << "\n6 enabled=" << (device.enabled ? "true" : "false") << "\n> ";
            unsigned choice = 0;
            if (!promptUnsigned("Device setting (0 to go back): ", 0, choice) || choice == 0) continue;
            unsigned number = 0;
            bool boolean = false;
            bool cancelled = false;
            switch (choice) {
            case 1: key = "address"; if (!promptUnsignedCancelable("New address [current] (B to go back): ", device.address, number, cancelled)) continue; value = static_cast<uint64_t>(number); break;
            case 2: key = "clock_hz"; if (!promptUnsignedCancelable("New clock [current] (B to go back): ", device.clock, number, cancelled)) continue; value = static_cast<uint64_t>(number); break;
            case 3: key = "refresh_seconds"; if (!promptUnsignedCancelable("New refresh [current] (B to go back): ", device.refreshSeconds, number, cancelled)) continue; value = static_cast<uint64_t>(number); break;
            case 4: key = "adapter_index"; if (!promptUnsignedCancelable("New adapter [current] (B to go back): ", device.adapterIndex, number, cancelled)) continue; value = static_cast<uint64_t>(number); break;
            case 5: key = "log_stream"; if (!promptBooleanCancelable("New logging (true/false, B to go back) [current]: ", device.logStream, boolean, cancelled)) continue; value = boolean; break;
            case 6: key = "enabled"; if (!promptBooleanCancelable("New enabled (true/false, B to go back) [current]: ", device.enabled, boolean, cancelled)) continue; value = boolean; break;
            default: std::cout << "Invalid setting.\n"; continue;
            }
            if (cancelled) { std::cout << "Setting update cancelled.\n"; continue; }
            std::string error;
            if (!app.updateDeviceSetting(selected - 1, key, value, error)) std::cout << "Setting update failed: " << error << "\n";
            else std::cout << "Saved [device " << device.name << "] " << key << ".\n";
        } else {
            std::cout << "Invalid category.\n";
        }
    }
}
}

// Initializes the service, displays the main menu, and dispatches user commands.
int main(int argc, char** argv) {
    const std::string configPath = argc > 1 ? argv[1] : defaultConfigPath();
    ApplicationService app;
    std::string error;
    if (!app.initialize(configPath, error)) {
        std::cerr << "Application initialization failed: " << error << "\n";
        std::cerr << "Usage: cp2112_console.exe [path-to-config.ini]\n";
        return 1;
    }

    showAdapters(app);
    if (app.config().devices.empty()) {
        std::cerr << "No configured devices are available.\n";
        return 1;
    }

    std::cout << "Version: " << versionString(app.config()) << "\n";

    while (true) {
        std::cout << "\n========== CP2112 Device Console " << versionString(app.config()) << " ==========\n"
                  << "1 List configured devices\n2 Select device\n3 Adapter information\n"
                  << "4 Reload configuration\n5 Scan I2C addresses\n"
                  << "6 Direct register read/write\n7 Clean old logs\n8 Settings\n0 Exit\n> ";
        int choice;
        if (!(std::cin >> choice)) {
            std::cin.clear();
            discardInputLine();
            continue;
        }
        if (choice == 0) break;
        if (choice == 1) {
            const AppConfig config = app.config();
            std::vector<size_t> enabled;
            for (size_t i = 0; i < config.devices.size(); ++i) {
                if (config.devices[i].enabled) enabled.push_back(i);
            }
            if (enabled.empty()) std::cout << "No enabled devices configured.\n";
            else showDevices(config, enabled);
        } else if (choice == 2) {
            selectDevice(app);
        } else if (choice == 3) {
            showAdapters(app);
        } else if (choice == 4) {
            if (!app.reload(error)) std::cout << "Configuration reload failed: " << error << "\n";
            else {
                const AppConfig config = app.config();
                const size_t enabled = static_cast<size_t>(std::count_if(
                    config.devices.begin(), config.devices.end(),
                    [](const DeviceConfig& device) { return device.enabled; }));
                std::cout << "Configuration reloaded. " << enabled << " of "
                          << config.devices.size() << " devices enabled.\n";
                if (!error.empty()) std::cout << error << "\n";
            }
        } else if (choice == 5) {
            scanBus(app);
        } else if (choice == 6) {
            directRegisterCommand(app);
        } else if (choice == 7) {
            cleanLogs(app);
        } else if (choice == 8) {
            settingsMenu(app);
        } else {
            std::cout << "Invalid choice.\n";
        }
    }
    return 0;
}

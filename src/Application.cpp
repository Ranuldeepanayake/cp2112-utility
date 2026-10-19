// Implements application configuration, hardware access, logging, streaming, and scans.
#include "Application.h"
#include "DriverRegistry.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace {
// Removes surrounding whitespace from configuration tokens.
std::string trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

// Normalizes case for case-insensitive configuration comparisons.
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// Gives every driver-backed audit entry a stable device and driver identity.
std::string driverAuditContext(const DeviceConfig& device, const Driver& driver) {
    return "device=\"" + device.name + "\" type=" + device.type +
           " driver=\"" + driver.title() + "\"";
}

// Converts the CP2112's compact transfer status into an audit-friendly cause.
std::string transferErrorDescription(const TransferDiagnostics& diagnostics) {
    std::string cause;
    if (diagnostics.timedOut || diagnostics.apiStatus == 0x12 || diagnostics.apiStatus == 0x13)
        cause = "timed_out";
    else if (diagnostics.detailedStatus == 0x00)
        cause = "address_nack_or_timeout";
    else if (diagnostics.detailedStatus == 0x01)
        cause = "bus_not_free_or_scl_low_timeout";
    else if (diagnostics.detailedStatus == 0x02)
        cause = "arbitration_lost_collision";
    else if (diagnostics.detailedStatus == 0x03)
        cause = "read_incomplete_frame_or_drop";
    else if (diagnostics.detailedStatus == 0x04)
        cause = "write_incomplete_frame_or_drop";
    else if (diagnostics.detailedStatus == 0x05)
        cause = "success_after_retry";
    else if (diagnostics.cancelled)
        cause = "cancelled";
    else
        cause = "transfer_error";
    return cause + " api_status=" + std::to_string(diagnostics.apiStatus) +
           " state=" + std::to_string(diagnostics.transferState) +
           " detail=" + std::to_string(diagnostics.detailedStatus) +
           " retries=" + std::to_string(diagnostics.retries) +
           " bytes=" + std::to_string(diagnostics.bytesTransferred) +
           " cancelled=" + (diagnostics.cancelled ? "true" : "false");
}

// Parses decimal or hexadecimal unsigned configuration values.
bool parseUnsigned(const std::string& text, unsigned long& value) {
    try {
        size_t consumed = 0;
        value = std::stoul(text, &consumed, 0);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

// Parses the supported textual boolean representations.
bool parseBool(const std::string& text, bool& value) {
    const std::string normalized = lower(trim(text));
    if (normalized == "true" || normalized == "yes" || normalized == "1" || normalized == "on") {
        value = true;
        return true;
    }
    if (normalized == "false" || normalized == "no" || normalized == "0" || normalized == "off") {
        value = false;
        return true;
    }
    return false;
}

// Loads and validates the INI-like configuration file and device definitions.
bool loadConfig(const std::string& path, AppConfig& config, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "Startup prerequisite warning: required config file is missing or unreadable: " + path;
        return false;
    }

    DeviceConfig* current = nullptr;
    std::string line;
    unsigned lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            const std::string section = trim(line.substr(1, line.size() - 2));
            if (lower(section).rfind("device ", 0) == 0) {
                config.devices.emplace_back();
                current = &config.devices.back();
                current->name = trim(section.substr(7));
                current->refreshSeconds = config.defaultRefreshSeconds;
                current->logStream = config.defaultLogStream;
            } else if (lower(section) == "general") {
                current = nullptr;
            } else {
                error = "Unknown section at line " + std::to_string(lineNumber);
                return false;
            }
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            error = "Expected key=value at line " + std::to_string(lineNumber);
            return false;
        }
        const std::string key = lower(trim(line.substr(0, equals)));
        const std::string value = trim(line.substr(equals + 1));
        unsigned long number = 0;
        bool boolean = false;
        if (!current) {
            if (key == "bridge_dll") config.bridgeDll = value;
            else if (key == "log_dir") config.logDir = value;
            else if (key == "version" && !value.empty()) config.version = value;
            else if (key == "build_timestamp" && !value.empty()) config.buildTimestamp = value;
            else if (key == "build_number" && !value.empty()) config.buildNumber = value;
            else if (key == "beta" && parseBool(value, boolean)) config.betaBuild = boolean;
            else if (key == "dark_mode" && parseBool(value, boolean)) config.darkMode = boolean;
            else if (key == "theme" && (lower(value) == "default" || lower(value) == "light" || lower(value) == "dark")) {
                config.theme = lower(value); config.darkMode = config.theme == "dark";
            }
            else if (key == "default_refresh_seconds" && parseUnsigned(value, number) && number >= 1 && number <= 3600)
                config.defaultRefreshSeconds = static_cast<unsigned>(number);
            else if (key == "default_log_stream" && parseBool(value, boolean))
                config.defaultLogStream = boolean;
            else if (key == "scan_adapter_index" && parseUnsigned(value, number) && number <= 255)
                config.scanAdapterIndex = static_cast<unsigned>(number);
            else if (key == "scan_start_address" && parseUnsigned(value, number) && number >= 0x08 && number <= 0x77)
                config.scanStartAddress = static_cast<unsigned>(number);
            else if (key == "scan_end_address" && parseUnsigned(value, number) && number >= 0x08 && number <= 0x77)
                config.scanEndAddress = static_cast<unsigned>(number);
            else if (key == "scan_clock_hz" && parseUnsigned(value, number) && number >= 1 && number <= 400000)
                config.scanClockHz = static_cast<unsigned>(number);
            else if (key == "transfer_timeout_ms" && parseUnsigned(value, number) && number >= 1 && number <= 1000)
                config.transferTimeoutMs = static_cast<unsigned>(number);
            else if (key == "transfer_retries" && parseUnsigned(value, number) && number <= 1000)
                config.transferRetries = static_cast<unsigned>(number);
            else if (key == "log_retention_days" && parseUnsigned(value, number) && number <= 36500)
                config.logRetentionDays = static_cast<unsigned>(number);
            else if (key == "gpio_activity_enabled" && parseBool(value, boolean))
                config.gpioActivityEnabled = boolean;
            else if (key == "gpio_error_pattern_enabled" && parseBool(value, boolean))
                config.gpioErrorPatternEnabled = boolean;
            else if (key == "gpio_error_pin" && parseUnsigned(value, number) && number >= 2 && number <= 6)
                config.gpioErrorPin = static_cast<unsigned>(number);
            else {
                error = "Invalid general setting at line " + std::to_string(lineNumber) + ": " + key;
                return false;
            }
        } else {
            if (key == "name") current->name = value;
            else if (key == "type") current->type = lower(value);
            else if (key == "address" && parseUnsigned(value, number) && number >= 0x08 && number <= 0x77)
                current->address = static_cast<unsigned>(number);
            else if (key == "clock_hz" && parseUnsigned(value, number) && number >= 1 && number <= 400000)
                current->clock = static_cast<DWORD>(number);
            else if (key == "refresh_seconds" && parseUnsigned(value, number) && number >= 1 && number <= 3600)
                current->refreshSeconds = static_cast<unsigned>(number);
            else if (key == "adapter_index" && parseUnsigned(value, number) && number <= 255)
                current->adapterIndex = static_cast<unsigned>(number);
            else if (key == "log_stream" && parseBool(value, boolean))
                current->logStream = boolean;
            else if (key == "enabled" && parseBool(value, boolean))
                current->enabled = boolean;
            else {
                error = "Invalid device setting at line " + std::to_string(lineNumber) +
                        ": " + key + "=" + value;
                return false;
            }
        }
    }

    if (config.devices.empty()) {
        error = "No [device NAME] sections found.";
        return false;
    }
    if (config.scanStartAddress > config.scanEndAddress) {
        error = "scan_start_address must be less than or equal to scan_end_address.";
        return false;
    }

    std::map<std::string, bool> names;
    for (const auto& device : config.devices) {
        if (device.name.empty() || device.type.empty() || device.address == 0) {
            error = "Each device requires name, type, and valid address.";
            return false;
        }
        const std::string normalizedName = lower(device.name);
        if (names[normalizedName]) {
            error = "Duplicate device name: " + device.name;
            return false;
        }
        names[normalizedName] = true;
        if (device.type != "bmp280" && device.type != "mpu6050" &&
            device.type != "bh1750" && device.type != "ds3231" &&
            device.type != "bq4050" && device.type != "24c32bn" && device.type != "ssd1306") {
            error = "Unsupported device type '" + device.type + "' for " + device.name;
            return false;
        }
    }
    return true;
}

// Retrieves one text property from the dynamically loaded CP2112 API.
std::string adapterString(Api& api, DWORD index, DWORD option) {
    char buffer[512]{};
    const Status status = api.getString(index, DEFAULT_VID, DEFAULT_PID, buffer, option);
    if (status != 0) return "(unavailable; API status " + std::to_string(status) + ")";
    return buffer;
}

// Confirms that a required path exists and is not a directory.
bool regularFileExists(const std::string& path) {
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Builds a path to a file located beside another path.
std::string siblingFile(const std::string& path, const std::string& filename) {
    const size_t separator = path.find_last_of("\\/");
    return separator == std::string::npos ? filename : path.substr(0, separator + 1) + filename;
}

// Identifies drive-qualified and root-qualified Windows paths.
bool isAbsolutePath(const std::string& path) {
    return (path.size() >= 2 && path[1] == ':') ||
           (!path.empty() && (path[0] == '\\' || path[0] == '/'));
}

// Converts a path to an absolute Windows path when possible.
std::string absolutePath(const std::string& path) {
    char buffer[MAX_PATH]{};
    const DWORD length = GetFullPathNameA(path.c_str(), MAX_PATH, buffer, nullptr);
    return length > 0 && length < MAX_PATH ? std::string(buffer, length) : path;
}

// Returns the directory portion of a path.
std::string directoryName(const std::string& path) {
    const size_t separator = path.find_last_of("\\/");
    return separator == std::string::npos ? std::string{} : path.substr(0, separator);
}

// Uses the package root for the standard config directory layout.
std::string packageBaseDirectory(const std::string& configPath) {
    const std::string configDirectory = directoryName(configPath);
    const size_t separator = configDirectory.find_last_of("\\/");
    const std::string folder = separator == std::string::npos
        ? configDirectory
        : configDirectory.substr(separator + 1);
    return lower(folder) == "config" ? directoryName(configDirectory) : configDirectory;
}

// Resolves package-relative paths against the package root.
std::string resolveRelativePath(const std::string& path, const std::string& baseDirectory) {
    return isAbsolutePath(path) ? path : absolutePath(baseDirectory + "\\" + path);
}

// Returns the directory containing the running frontend executable.
std::string executableDirectory() {
    char buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    const std::string executablePath(buffer, length);
    const size_t separator = executablePath.find_last_of("\\/");
    return separator == std::string::npos ? std::string{} : executablePath.substr(0, separator);
}

// Resolves the common config path and falls back beside the executable when
// the package intentionally keeps frontend-specific copies in bin\cli or bin\gui.
std::string resolveBridgeDll(const std::string& configuredPath, const std::string& packageDirectory) {
    const std::string packagePath = resolveRelativePath(configuredPath, packageDirectory);
    if (regularFileExists(packagePath)) return packagePath;
    const size_t separator = configuredPath.find_last_of("\\/");
    const std::string filename = separator == std::string::npos
        ? configuredPath
        : configuredPath.substr(separator + 1);
    const std::string executableDir = executableDirectory();
    const std::string executablePath = executableDir.empty()
        ? std::string{}
        : executableDir + "\\" + filename;
    return !executablePath.empty() && regularFileExists(executablePath)
        ? executablePath
        : packagePath;
}

bool settingUnsigned(const SettingValue& value, uint64_t& result) {
    if (!std::holds_alternative<uint64_t>(value)) return false;
    result = std::get<uint64_t>(value);
    return true;
}

bool settingBoolean(const SettingValue& value, bool& result) {
    if (!std::holds_alternative<bool>(value)) return false;
    result = std::get<bool>(value);
    return true;
}

bool settingString(const SettingValue& value, std::string& result) {
    if (!std::holds_alternative<std::string>(value)) return false;
    result = std::get<std::string>(value);
    return !result.empty();
}

// Creates a directory and any missing parent directories using Windows APIs.
bool ensureDirectory(const std::string& path, std::string& error) {
    if (path.empty()) {
        error = "Log directory path is empty.";
        return false;
    }

    const DWORD existingAttributes = GetFileAttributesA(path.c_str());
    if (existingAttributes != INVALID_FILE_ATTRIBUTES) {
        if ((existingAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return true;
        error = "The log directory path is a file: " + path;
        return false;
    }

    const size_t separator = path.find_last_of("\\/");
    if (separator != std::string::npos && separator > 0) {
        const std::string parent = path.substr(0, separator);
        if (!ensureDirectory(parent, error)) return false;
    }

    if (CreateDirectoryA(path.c_str(), nullptr)) return true;
    const DWORD status = GetLastError();
    if (status == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return true;
    }
    error = "Cannot create log directory '" + path + "' (Windows error " +
            std::to_string(status) + ")";
    return false;
}

// Recognizes the timestamped audit and stream log naming patterns.
bool isTimestampedLog(const std::string& name) {
    const std::string filename = lower(name);
    const bool audit = filename.rfind("audit_", 0) == 0 &&
        filename.compare(filename.size() - 4, 4, ".log") == 0;
    const bool stream = filename.compare(filename.size() - 4, 4, ".csv") == 0;
    if ((!audit && !stream) || filename.size() < 15 + 4) return false;
    const size_t digitsStart = filename.size() - 4 - 15;
    for (size_t i = digitsStart; i < digitsStart + 15; ++i) {
        if (i == digitsStart + 8) {
            if (filename[i] != '_') return false;
        } else if (!std::isdigit(static_cast<unsigned char>(filename[i]))) {
            return false;
        }
    }
    return true;
}

// Removes eligible old logs while preserving the active audit log.
LogCleanupResult removeOldLogs(const std::string& directory, unsigned days,
                               const std::string& activeAuditPath) {
    struct Candidate {
        std::string path;
        FILETIME modified;
    };
    std::vector<Candidate> candidates;
    const std::string pattern = directory + "\\*";
    WIN32_FIND_DATAA entry{};
    HANDLE search = FindFirstFileA(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD status = GetLastError();
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return {};
        return {0, 1};
    }

    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            !isTimestampedLog(entry.cFileName)) continue;
        const std::string path = directory + "\\" + entry.cFileName;
        if (lower(path) == lower(activeAuditPath)) continue;
        candidates.push_back({path, entry.ftLastWriteTime});
    } while (FindNextFileA(search, &entry));
    const DWORD enumerationStatus = GetLastError();
    FindClose(search);

    FILETIME currentTime{};
    GetSystemTimeAsFileTime(&currentTime);
    ULARGE_INTEGER now{};
    now.LowPart = currentTime.dwLowDateTime;
    now.HighPart = currentTime.dwHighDateTime;
    const ULONGLONG ticksPerDay = 86400ULL * 10000000ULL;
    const ULONGLONG ageTicks = static_cast<ULONGLONG>(days) * ticksPerDay;
    const ULONGLONG cutoff = now.QuadPart > ageTicks ? now.QuadPart - ageTicks : 0;
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return CompareFileTime(&left.modified, &right.modified) < 0;
    });

    LogCleanupResult result;
    for (const auto& candidate : candidates) {
        ULARGE_INTEGER modified{};
        modified.LowPart = candidate.modified.dwLowDateTime;
        modified.HighPart = candidate.modified.dwHighDateTime;
        if (modified.QuadPart > cutoff) continue;
        if (DeleteFileA(candidate.path.c_str())) ++result.deleted;
        else ++result.failed;
    }
    if (enumerationStatus != ERROR_NO_MORE_FILES) ++result.failed;
    return result;
}

class DeviceHandle {
public:
    explicit DeviceHandle(Api& api) : api_(api) {}
    ~DeviceHandle() { if (handle_) api_.close(handle_); }
    Device* address() { return &handle_; }
    Device get() const { return handle_; }
private:
    Api& api_;
    Device handle_ = nullptr;
};
}

ApplicationService::ApplicationService() = default;
ApplicationService::~ApplicationService() = default;

// Loads startup configuration, creates logging, validates prerequisites, and opens the CP2112 API.
bool ApplicationService::initialize(const std::string& configPath, std::string& error,
                                    const std::string& frontendTag) {
    error.clear();
    configPath_ = absolutePath(configPath);
    AppConfig loaded;
    if (!loadConfig(configPath_, loaded, error)) return false;
    const std::string packageDirectory = packageBaseDirectory(configPath_);
    loaded.bridgeDll = resolveBridgeDll(loaded.bridgeDll, packageDirectory);
    loaded.logDir = resolveRelativePath(loaded.logDir, packageDirectory);
    config_ = std::move(loaded);

    std::string logDirectoryError;
    if (!ensureDirectory(config_.logDir, logDirectoryError)) {
        error = "Startup prerequisite warning: " + logDirectoryError;
        return false;
    }

    std::vector<std::string> missingFiles;
    if (!regularFileExists(config_.bridgeDll)) missingFiles.push_back(config_.bridgeDll);
    const std::string deviceDll = siblingFile(config_.bridgeDll, "SLABHIDDevice.dll");
    if (!regularFileExists(deviceDll)) missingFiles.push_back(deviceDll);
    if (!missingFiles.empty()) {
        error = "Startup prerequisite warning: required DLL file(s) are missing:";
        for (const std::string& file : missingFiles) error += "\n - " + file;
        return false;
    }

    logger_ = std::make_unique<Logger>(config_.logDir, frontendTag);
    if (!logger_->available()) {
        error = "Startup prerequisite warning: cannot create an audit log in '" +
                config_.logDir + "'.";
        return false;
    }
    logger_->audit("Application started; config=" + configPath_);
    logger_->audit("Registered device definitions loaded: " + std::to_string(config_.devices.size()));
    if (config_.logRetentionDays > 0) {
        const LogCleanupResult cleanup = removeOldLogs(config_.logDir, config_.logRetentionDays, logger_->auditPath());
        logger_->audit("Automatic log cleanup: deleted " + std::to_string(cleanup.deleted) +
                       " old log(s); failures=" + std::to_string(cleanup.failed));
    }
    if (!api_.init(config_.bridgeDll, error)) return false;
    if (api_.getNumDevices(&adapterCount_, DEFAULT_VID, DEFAULT_PID) != 0) {
        error = "Could not enumerate CP2112 adapters";
        return false;
    }
    initialized_ = true;
    return true;
}

// Returns a thread-safe snapshot of the current application configuration.
AppConfig ApplicationService::config() const {
    AppLockGuard lock(configMutex_);
    return config_;
}

// Reloads editable configuration values while retaining the active hardware session.
bool ApplicationService::reload(std::string& error) {
    error.clear();
    AppConfig loaded;
    if (!loadConfig(configPath_, loaded, error)) {
        if (logger_) logger_->audit("Configuration reload failed: " + error);
        return false;
    }
    const std::string packageDirectory = packageBaseDirectory(configPath_);
    loaded.bridgeDll = resolveBridgeDll(loaded.bridgeDll, packageDirectory);
    loaded.logDir = resolveRelativePath(loaded.logDir, packageDirectory);
    bool restartRequired = false;
    size_t enabled = 0;
    size_t deviceCount = 0;
    {
        AppLockGuard lock(configMutex_);
        restartRequired = loaded.bridgeDll != config_.bridgeDll || loaded.logDir != config_.logDir;
        config_.devices.swap(loaded.devices);
        config_.defaultRefreshSeconds = loaded.defaultRefreshSeconds;
        config_.defaultLogStream = loaded.defaultLogStream;
        config_.scanAdapterIndex = loaded.scanAdapterIndex;
        config_.scanStartAddress = loaded.scanStartAddress;
        config_.scanEndAddress = loaded.scanEndAddress;
        config_.scanClockHz = loaded.scanClockHz;
        config_.transferRetries = loaded.transferRetries;
        config_.buildTimestamp = loaded.buildTimestamp;
        config_.buildNumber = loaded.buildNumber;
        config_.betaBuild = loaded.betaBuild;
        config_.transferTimeoutMs = loaded.transferTimeoutMs;
        config_.logRetentionDays = loaded.logRetentionDays;
        deviceCount = config_.devices.size();
        enabled = static_cast<size_t>(std::count_if(
            config_.devices.begin(), config_.devices.end(),
            [](const DeviceConfig& device) { return device.enabled; }));
    }
    if (logger_) {
        logger_->audit("Configuration reloaded: " + std::to_string(enabled) +
                       " of " + std::to_string(deviceCount) + " devices enabled");
    }
    if (restartRequired) {
        error = "Device definitions reloaded; bridge_dll and log_dir changes require restart.";
    }
    return true;
}

// Updates one existing INI setting without rewriting unrelated comments or sections.
bool ApplicationService::persistSetting(const std::string& section, const std::string& key,
                                        const std::string& value, std::string& error) const {
    std::ifstream input(configPath_);
    if (!input) { error = "Could not read configuration file: " + configPath_; return false; }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    const std::string expectedSection = lower(trim(section));
    const std::string expectedKey = lower(trim(key));
    std::string currentSection;
    bool replaced = false;
    for (std::string& currentLine : lines) {
        const std::string normalized = trim(currentLine);
        if (normalized.size() >= 2 && normalized.front() == '[' && normalized.back() == ']') {
            currentSection = lower(trim(normalized.substr(1, normalized.size() - 2)));
            continue;
        }
        const size_t equals = normalized.find('=');
        if (!replaced && currentSection == expectedSection && equals != std::string::npos &&
            lower(trim(normalized.substr(0, equals))) == expectedKey) {
            currentLine = key + "=" + value;
            replaced = true;
        }
    }
    if (!replaced) {
        error = "Configuration setting was not found: [" + section + "] " + key;
        return false;
    }

    const std::string temporaryPath = configPath_ + ".tmp";
    const auto writeLines = [&lines](const std::string& path) {
        std::ofstream output(path, std::ios::trunc);
        if (!output) return false;
        for (const std::string& outputLine : lines) output << outputLine << "\n";
        return static_cast<bool>(output);
    };
    {
        std::ofstream output(temporaryPath, std::ios::trunc);
        if (!output) { error = "Could not create temporary configuration file: " + temporaryPath; return false; }
        for (const std::string& outputLine : lines) output << outputLine << "\n";
        if (!output) {
            output.close();
            DeleteFileA(temporaryPath.c_str());
            error = "Could not write temporary configuration file: " + temporaryPath;
            return false;
        }
    }
    if (!MoveFileExA(temporaryPath.c_str(), configPath_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD replaceError = GetLastError();
        // Some Windows processes temporarily deny replacement of an open INI file.
        // Fall back to truncating the same file so settings can still be saved.
        if (replaceError == ERROR_ACCESS_DENIED) {
            const DWORD attributes = GetFileAttributesA(configPath_.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
                SetFileAttributesA(configPath_.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
            }
            if (writeLines(configPath_)) {
                DeleteFileA(temporaryPath.c_str());
                return true;
            }
        }
        DeleteFileA(temporaryPath.c_str());
        error = "Could not replace configuration file (Windows error " +
                std::to_string(replaceError) + ")";
        return false;
    }
    return true;
}

// Validates, persists, and applies an editable general setting.
bool ApplicationService::updateGeneralSetting(const std::string& key, const SettingValue& value,
                                               std::string& error) {
    error.clear();
    if (!initialized_) { error = "Application service is not initialized"; return false; }
    const std::string normalized = lower(trim(key));
    AppLockGuard lock(configMutex_);
    const AppConfig previous = config_;
    std::string serialized;
    std::string text;
    uint64_t number = 0;
    bool boolean = false;
    if (normalized == "bridge_dll" && settingString(value, text)) {
        // Keep the standard package setting portable when a frontend displays
        // the resolved copy from bin\cli or bin\gui and saves it unchanged.
        const std::string normalizedText = lower(text);
        const bool isStandardBridge = normalizedText.find("slabhidtosmbus.dll") != std::string::npos &&
            (text == config_.bridgeDll || normalizedText.find("\\bin\\cli\\") != std::string::npos ||
             normalizedText.find("/bin/cli/") != std::string::npos ||
             normalizedText.find("\\bin\\gui\\") != std::string::npos ||
             normalizedText.find("/bin/gui/") != std::string::npos);
        serialized = isStandardBridge ? "bin\\SLABHIDtoSMBus.dll" : text;
        config_.bridgeDll = resolveBridgeDll(serialized, packageBaseDirectory(configPath_));
    } else if (normalized == "log_dir" && settingString(value, text)) {
        serialized = text; config_.logDir = resolveRelativePath(text, packageBaseDirectory(configPath_));
    } else if (normalized == "default_refresh_seconds" && settingUnsigned(value, number) && number >= 1 && number <= 3600) {
        serialized = std::to_string(number); config_.defaultRefreshSeconds = static_cast<unsigned>(number);
    } else if (normalized == "default_log_stream" && settingBoolean(value, boolean)) {
        serialized = boolean ? "true" : "false"; config_.defaultLogStream = boolean;
    } else if (normalized == "dark_mode" && settingBoolean(value, boolean)) {
        serialized = boolean ? "true" : "false"; config_.darkMode = boolean; config_.theme = boolean ? "dark" : "default";
    } else if (normalized == "theme" && settingString(value, text) &&
               (lower(text) == "default" || lower(text) == "light" || lower(text) == "dark")) {
        serialized = lower(text); config_.theme = serialized; config_.darkMode = serialized == "dark";
    } else if (normalized == "scan_adapter_index" && settingUnsigned(value, number) && number <= 255) {
        serialized = std::to_string(number); config_.scanAdapterIndex = static_cast<unsigned>(number);
    } else if (normalized == "scan_start_address" && settingUnsigned(value, number) && number >= 0x08 && number <= config_.scanEndAddress) {
        serialized = hex(static_cast<unsigned>(number)); config_.scanStartAddress = static_cast<unsigned>(number);
    } else if (normalized == "scan_end_address" && settingUnsigned(value, number) && number >= config_.scanStartAddress && number <= 0x77) {
        serialized = hex(static_cast<unsigned>(number)); config_.scanEndAddress = static_cast<unsigned>(number);
    } else if (normalized == "scan_clock_hz" && settingUnsigned(value, number) && number >= 1 && number <= 400000) {
        serialized = std::to_string(number); config_.scanClockHz = static_cast<unsigned>(number);
    } else if (normalized == "transfer_timeout_ms" && settingUnsigned(value, number) && number >= 1 && number <= 1000) {
        serialized = std::to_string(number); config_.transferTimeoutMs = static_cast<unsigned>(number);
    } else if (normalized == "transfer_retries" && settingUnsigned(value, number) && number <= 1000) {
        serialized = std::to_string(number); config_.transferRetries = static_cast<unsigned>(number);
    } else if (normalized == "log_retention_days" && settingUnsigned(value, number) && number <= 36500) {
        serialized = std::to_string(number); config_.logRetentionDays = static_cast<unsigned>(number);
    } else if (normalized == "gpio_activity_enabled" && settingBoolean(value, boolean)) {
        serialized = boolean ? "true" : "false"; config_.gpioActivityEnabled = boolean;
    } else if (normalized == "gpio_error_pattern_enabled" && settingBoolean(value, boolean)) {
        serialized = boolean ? "true" : "false"; config_.gpioErrorPatternEnabled = boolean;
    } else if (normalized == "gpio_error_pin" && settingUnsigned(value, number) && number >= 2 && number <= 6) {
        serialized = std::to_string(number); config_.gpioErrorPin = static_cast<unsigned>(number);
    } else {
        error = "Invalid or read-only general setting: " + key;
        return false;
    }
    if (!persistSetting("general", normalized, serialized, error)) {
        config_ = previous;
        return false;
    }
    if (logger_) logger_->audit("General setting updated: " + normalized + "=" + serialized);
    return true;
}

// Validates, persists, and applies an editable device setting.
bool ApplicationService::updateDeviceSetting(size_t deviceIndex, const std::string& key,
                                              const SettingValue& value, std::string& error) {
    error.clear();
    if (!initialized_) { error = "Application service is not initialized"; return false; }
    const std::string normalized = lower(trim(key));
    AppLockGuard lock(configMutex_);
    if (deviceIndex >= config_.devices.size()) { error = "Device index is out of range"; return false; }
    DeviceConfig& device = config_.devices[deviceIndex];
    const DeviceConfig previous = device;
    std::string serialized;
    uint64_t number = 0;
    bool boolean = false;
    if (normalized == "address" && settingUnsigned(value, number) && number >= 0x08 && number <= 0x77) {
        serialized = hex(static_cast<unsigned>(number)); device.address = static_cast<unsigned>(number);
    } else if (normalized == "clock_hz" && settingUnsigned(value, number) && number >= 1 && number <= 400000) {
        serialized = std::to_string(number); device.clock = static_cast<DWORD>(number);
    } else if (normalized == "refresh_seconds" && settingUnsigned(value, number) && number >= 1 && number <= 3600) {
        serialized = std::to_string(number); device.refreshSeconds = static_cast<unsigned>(number);
    } else if (normalized == "adapter_index" && settingUnsigned(value, number) && number <= 255) {
        serialized = std::to_string(number); device.adapterIndex = static_cast<unsigned>(number);
    } else if (normalized == "log_stream" && settingBoolean(value, boolean)) {
        serialized = boolean ? "true" : "false"; device.logStream = boolean;
    } else if (normalized == "enabled" && settingBoolean(value, boolean)) {
        serialized = boolean ? "true" : "false"; device.enabled = boolean;
    } else {
        error = "Invalid or read-only device setting: " + key;
        return false;
    }
    if (!persistSetting("device " + device.name, normalized, serialized, error)) {
        device = previous;
        return false;
    }
    if (logger_) logger_->audit("Device setting updated: " + device.name + ": " + normalized + "=" + serialized);
    return true;
}

// Enumerates connected CP2112 adapters and collects their descriptive properties.
bool ApplicationService::adapters(std::vector<AdapterInformation>& result, std::string& error) {
    error.clear();
    if (!initialized_) { error = "Application service is not initialized"; return false; }
    AppLockGuard hardwareLock(hardwareMutex_);
    DWORD count = 0;
    const Status status = api_.getNumDevices(&count, DEFAULT_VID, DEFAULT_PID);
    if (status != 0) {
        error = "GetNumDevices failed: " + std::to_string(status);
        return false;
    }
    adapterCount_ = count;
    result.clear();
    result.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
        AdapterInformation info;
        info.index = index;
        info.product = adapterString(api_, index, 0x06);
        info.manufacturer = adapterString(api_, index, 0x05);
        info.serial = adapterString(api_, index, 0x04);
        info.devicePath = adapterString(api_, index, 0x03);
        info.attributeStatus = api_.getAttributes(index, DEFAULT_VID, DEFAULT_PID,
                                                  &info.vid, &info.pid, &info.release);
        info.attributesAvailable = info.attributeStatus == 0;

        // Read device and library metadata without changing any persistent
        // USB configuration. Each adapter must be opened for part details.
        DeviceHandle handle(api_);
        const Status openStatus = api_.open(handle.address(), index, DEFAULT_VID, DEFAULT_PID);
        if (openStatus == 0 && handle.get()) {
            info.partNumberStatus = api_.getPartNumber(handle.get(), &info.partNumber, &info.partVersion);
            info.partNumberAvailable = info.partNumberStatus == 0;
            info.usbConfigStatus = api_.getUsbConfig(handle.get(), &info.configuredVid,
                                                     &info.configuredPid, &info.configuredPower,
                                                     &info.configuredPowerMode, &info.configuredRelease);
            info.usbConfigAvailable = info.usbConfigStatus == 0;
            info.lockStatus = api_.getLock(handle.get(), &info.lockBits);
            info.lockAvailable = info.lockStatus == 0;
            info.smbusConfigStatus = api_.getSmbusConfig(
                handle.get(), &info.smbusBitRate, &info.smbusAddress,
                &info.smbusAutoReadRespond, &info.smbusWriteTimeout,
                &info.smbusReadTimeout, &info.smbusSclLowTimeout,
                &info.smbusTransferRetries);
            info.smbusConfigAvailable = info.smbusConfigStatus == 0;
        } else {
            info.partNumberStatus = openStatus;
            info.usbConfigStatus = openStatus;
            info.lockStatus = openStatus;
            info.smbusConfigStatus = openStatus;
        }
        info.smbusLibraryVersionStatus = api_.getLibraryVersion(
            &info.smbusLibraryMajor, &info.smbusLibraryMinor, &info.smbusLibraryRelease);
        info.smbusLibraryVersionAvailable = info.smbusLibraryVersionStatus == 0;
        info.hidLibraryVersionStatus = api_.getHidLibraryVersion(
            &info.hidLibraryMajor, &info.hidLibraryMinor, &info.hidLibraryRelease);
        info.hidLibraryVersionAvailable = info.hidLibraryVersionStatus == 0;
        result.push_back(std::move(info));
    }
    return true;
}

// Retrieves and validates an enabled configured device by index.
bool ApplicationService::getDevice(size_t deviceIndex, DeviceConfig& device,
                                   std::string& error) const {
    if (!initialized_) { error = "Application service is not initialized"; return false; }
    AppLockGuard lock(configMutex_);
    if (deviceIndex >= config_.devices.size()) { error = "Device index is out of range"; return false; }
    device = config_.devices[deviceIndex];
    if (!device.enabled) { error = "Device is disabled in configuration"; return false; }
    return true;
}

// Opens the configured adapter for a device operation.
bool ApplicationService::openDevice(const DeviceConfig& config, Device& handle,
                                    std::string& error) {
    if (config.adapterIndex >= adapterCount_) {
        error = "Configured adapter index is out of range for " + config.name;
        return false;
    }
    const Status status = api_.open(&handle, config.adapterIndex, DEFAULT_VID, DEFAULT_PID);
    if (status != 0 || !handle) {
        error = "Failed to open CP2112 adapter index " + std::to_string(config.adapterIndex) +
                "; status=" + std::to_string(status);
        return false;
    }
    if (!configureGpio(handle, config_.gpioActivityEnabled, config_.gpioErrorPatternEnabled,
                       error)) {
        api_.close(handle);
        handle = nullptr;
        return false;
    }
    return true;
}

// Applies the reusable indicator policy. GPIO0/1 use CP2112 hardware TX/RX
// toggle functions; the selected error pin is a normal push-pull GPIO so a
// future frontend or board-specific mapping can reuse the same service.
bool ApplicationService::configureGpio(Device handle, bool activity, bool errorOutput,
                                       std::string& error) {
    if (!handle || !api_.setGpioConfig) { error = "GPIO API is unavailable"; return false; }
    // Keep GPIO0/1 explicitly configured as push-pull outputs as well as
    // selecting their special functions. This matches the CP2112 evaluation
    // board LED wiring and preserves strong TX/RX activity indication.
    BYTE direction = activity ? 0x03 : 0;
    BYTE mode = activity ? 0x03 : 0;
    BYTE function = activity ? 0x06 : 0x00;
    if (errorOutput && config_.gpioErrorPin <= 7) {
        const BYTE errorMask = static_cast<BYTE>(1u << config_.gpioErrorPin);
        direction = static_cast<BYTE>(direction | errorMask);
        mode = static_cast<BYTE>(mode | errorMask);
    }
    if (api_.setGpioConfig(handle, direction, mode, function, 0) != 0) {
        error = "Could not configure CP2112 GPIO indicators";
        return false;
    }
    return true;
}

bool ApplicationService::gpioState(GpioIndicatorState& state, std::string& error) {
    error.clear();
    AppLockGuard lock(hardwareMutex_);
    if (!initialized_ || config_.scanAdapterIndex >= adapterCount_) { error = "No CP2112 adapter is available"; return false; }
    DeviceHandle handle(api_);
    if (api_.open(handle.address(), config_.scanAdapterIndex, DEFAULT_VID, DEFAULT_PID) != 0) {
        error = "Could not open CP2112 adapter"; return false;
    }
    if (api_.getGpioConfig(handle.get(), &state.direction, &state.mode, &state.function,
                           &state.clockDivider) != 0 || api_.readLatch(handle.get(), &state.latch) != 0) {
        error = "Could not read CP2112 GPIO state"; return false;
    }
    return true;
}

bool ApplicationService::setGpioActivityIndicators(bool enabled, std::string& error) {
    if (!updateGeneralSetting("gpio_activity_enabled", enabled, error)) return false;
    AppLockGuard lock(hardwareMutex_);
    DeviceHandle handle(api_);
    if (config_.scanAdapterIndex >= adapterCount_ || api_.open(handle.address(), config_.scanAdapterIndex, DEFAULT_VID, DEFAULT_PID) != 0) {
        error = "Could not open CP2112 adapter"; return false;
    }
    const bool ok = configureGpio(handle.get(), enabled, config_.gpioErrorPatternEnabled, error);
    if (logger_) logger_->audit("GPIO action=activity_indicators enabled=" + std::string(enabled ? "true" : "false") +
                                " succeeded=" + (ok ? "true" : "false") + (error.empty() ? "" : " error=\"" + error + "\""));
    return ok;
}

bool ApplicationService::setGpioErrorPattern(bool enabled, std::string& error) {
    if (!updateGeneralSetting("gpio_error_pattern_enabled", enabled, error)) return false;
    AppLockGuard lock(hardwareMutex_);
    DeviceHandle handle(api_);
    if (config_.scanAdapterIndex >= adapterCount_ || api_.open(handle.address(), config_.scanAdapterIndex, DEFAULT_VID, DEFAULT_PID) != 0) {
        error = "Could not open CP2112 adapter"; return false;
    }
    const bool ok = configureGpio(handle.get(), config_.gpioActivityEnabled, enabled, error);
    if (logger_) logger_->audit("GPIO action=error_pattern enabled=" + std::string(enabled ? "true" : "false") +
                                " pin=" + std::to_string(config_.gpioErrorPin) +
                                " succeeded=" + (ok ? "true" : "false") + (error.empty() ? "" : " error=\"" + error + "\""));
    return ok;
}

bool ApplicationService::testGpioIndicators(std::string& error) {
    error.clear();
    AppLockGuard lock(hardwareMutex_);
    DeviceHandle handle(api_);
    if (config_.scanAdapterIndex >= adapterCount_ || api_.open(handle.address(), config_.scanAdapterIndex, DEFAULT_VID, DEFAULT_PID) != 0) {
        error = "Could not open CP2112 adapter"; return false;
    }
    const BYTE mask = 0x03;
    if (api_.setGpioConfig(handle.get(), mask, mask, 0, 0) != 0) { error = "Could not configure test GPIOs"; if (logger_) logger_->audit("GPIO action=test_leds succeeded=false error=\"" + error + "\""); return false; }
    const BYTE testValues[] = {0x00, 0x01, 0x02, 0x03, 0x00};
    for (BYTE value : testValues) {
        if (api_.writeLatch(handle.get(), value, mask) != 0) { error = "Could not drive test GPIOs"; if (logger_) logger_->audit("GPIO action=test_leds succeeded=false error=\"" + error + "\""); return false; }
        Sleep(150);
    }
    // The evaluation-board LEDs are normally wired to the CP2112 special
    // functions, so also exercise the real TX/RX toggle path. A harmless
    // one-byte read is attempted against the first configured target; a
    // missing target is acceptable because the purpose is LED activity.
    if (config_.gpioActivityEnabled) {
        if (api_.setGpioConfig(handle.get(), 0, 0x00, 0x06, 0) != 0) {
            error = "Could not enable TX/RX toggle functions for LED test";
            if (logger_) logger_->audit("GPIO action=test_leds succeeded=false error=\"" + error + "\"");
            return false;
        }
        unsigned testAddress = 0x7F;
        for (const auto& device : config_.devices) {
            if (device.enabled) { testAddress = device.address; break; }
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (api_.readRequest(handle.get(), static_cast<BYTE>(testAddress << 1), 1) == 0) {
                BYTE response[61]{}, received = 0; int state = TRANSFER_IDLE;
                api_.getReadResponse(handle.get(), &state, response, sizeof(response), &received);
            }
            Sleep(100);
        }
    }
    const bool ok = configureGpio(handle.get(), config_.gpioActivityEnabled, config_.gpioErrorPatternEnabled, error);
    if (logger_) logger_->audit("GPIO action=test_leds pins=GPIO0,GPIO1 succeeded=" + std::string(ok ? "true" : "false") +
                                (error.empty() ? "" : " error=\"" + error + "\""));
    return ok;
}

bool ApplicationService::signalGpioError(Device handle) {
    if (!config_.gpioErrorPatternEnabled || !handle) return false;
    const BYTE mask = static_cast<BYTE>(1u << config_.gpioErrorPin);
    const BYTE activityMask = config_.gpioActivityEnabled ? 0x03 : 0;
    if (api_.setGpioConfig(handle, static_cast<BYTE>(mask | activityMask),
                           static_cast<BYTE>(mask | activityMask),
                           config_.gpioActivityEnabled ? 0x06 : 0, 0) != 0) return false;
    // Short Morse-like error indication: three flashes, pause, one flash.
    for (int i = 0; i < 3; ++i) { api_.writeLatch(handle, mask, mask); Sleep(60); api_.writeLatch(handle, 0, mask); Sleep(60); }
    Sleep(100);
    api_.writeLatch(handle, mask, mask); Sleep(180); api_.writeLatch(handle, 0, mask);
    std::string ignored;
    configureGpio(handle, config_.gpioActivityEnabled, config_.gpioErrorPatternEnabled, ignored);
    if (logger_) logger_->audit("GPIO action=error_pattern_flash pin=" + std::to_string(config_.gpioErrorPin) + " pattern=3-short-1-long");
    return true;
}

// Performs one driver-defined read for a configured device.
bool ApplicationService::readDeviceResult(size_t deviceIndex, DeviceResult& result,
                                          std::string& error) {
    error.clear();
    result = {};
    DeviceConfig device;
    if (!getDevice(deviceIndex, device, error)) return false;
    std::unique_ptr<Driver> driver = makeDriver(device.type);
    if (!driver) {
        error = "No driver registered for " + device.type;
        if (logger_) logger_->audit("Driver-backed read failed device=\"" + device.name +
                                    "\" type=" + device.type + " reason=\"" + error + "\"");
        return false;
    }
    AppLockGuard hardwareLock(hardwareMutex_);
    DeviceHandle handle(api_);
    if (!openDevice(device, *handle.address(), error)) {
        if (logger_) logger_->audit(driverAuditContext(device, *driver) +
                                   " operation=read succeeded=false stage=open error=\"" + error + "\"");
        return false;
    }
    Bus bus(api_, handle.get(), config_.transferTimeoutMs, config_.transferRetries);
    if (!bus.setClock(device.clock)) {
        error = "Failed to set bus clock for " + device.name;
        if (logger_) logger_->audit(driverAuditContext(device, *driver) +
                                   " operation=read succeeded=false stage=configure error=\"" + error + "\"");
        return false;
    }
    const bool succeeded = driver->readResult(bus, device, result);
    if (!succeeded) {
        signalGpioError(handle.get());
        if (logger_) logger_->audit(driverAuditContext(device, *driver) + " i2c_error=" + transferErrorDescription(bus.lastDiagnostics()));
    }
    if (logger_) logger_->audit(driverAuditContext(device, *driver) +
                               " operation=read succeeded=" + (succeeded ? "true" : "false") +
                               " result=\"" + formatDeviceResult(result) + "\"");
    return succeeded;
}

// Formats the structured result for the existing console-facing API.
bool ApplicationService::readDevice(size_t deviceIndex, std::string& result, std::string& error) {
    DeviceResult structured;
    const bool succeeded = readDeviceResult(deviceIndex, structured, error);
    result = formatDeviceResult(structured);
    return succeeded;
}

// Returns the metadata and actions exposed by a configured device driver.
bool ApplicationService::driverInformation(size_t deviceIndex, DriverInformation& result,
                                           std::string& error) const {
    error.clear();
    DeviceConfig device;
    if (!getDevice(deviceIndex, device, error)) return false;
    std::unique_ptr<Driver> driver = makeDriver(device.type);
    if (!driver) { error = "No driver registered for " + device.type; return false; }
    result.title = driver->title();
    result.capabilities = driver->capabilities();
    result.actions = driver->actions();
    result.registers = driver->registers();
    return true;
}

// Executes a driver-defined action after opening and configuring the bus.
bool ApplicationService::executeDeviceAction(size_t deviceIndex, const std::string& action,
                                             const std::vector<std::string>& parameters,
                                             std::string& result, std::string& error) {
    error.clear();
    result.clear();
    DeviceConfig device;
    if (!getDevice(deviceIndex, device, error)) return false;
    std::unique_ptr<Driver> driver = makeDriver(device.type);
    if (!driver) {
        error = "No driver registered for " + device.type;
        if (logger_) logger_->audit("Driver-backed action failed device=\"" + device.name +
                                    "\" type=" + device.type + " action=" + action +
                                    " reason=\"" + error + "\"");
        return false;
    }
    AppLockGuard hardwareLock(hardwareMutex_);
    DeviceHandle handle(api_);
    if (!openDevice(device, *handle.address(), error)) {
        if (logger_) logger_->audit(driverAuditContext(device, *driver) +
                                   " operation=action action=" + action +
                                   " succeeded=false stage=open error=\"" + error + "\"");
        return false;
    }
    Bus bus(api_, handle.get(), config_.transferTimeoutMs, config_.transferRetries);
    if (!bus.setClock(device.clock)) {
        error = "Failed to set bus clock for " + device.name;
        if (logger_) logger_->audit(driverAuditContext(device, *driver) +
                                   " operation=action action=" + action +
                                   " succeeded=false stage=configure error=\"" + error + "\"");
        return false;
    }
    const bool succeeded = driver->executeAction(bus, device, action, parameters, result);
    if (!succeeded) {
        signalGpioError(handle.get());
        if (logger_) logger_->audit(driverAuditContext(device, *driver) + " action=" + action +
                                   " i2c_error=" + transferErrorDescription(bus.lastDiagnostics()));
    }
    if (logger_) logger_->audit(driverAuditContext(device, *driver) +
                               " operation=action action=" + action +
                               " succeeded=" + (succeeded ? "true" : "false") +
                               " result=\"" + result + "\"");
    return succeeded;
}

// Reads a bounded response from an arbitrary register using the scan adapter.
// The requested length is part of the transfer contract: CP2112 uses it to
// complete the SMBus read and generate the final bus response condition.
bool ApplicationService::readRegister(unsigned deviceAddress, unsigned registerAddress,
                                      unsigned expectedBytes, std::vector<BYTE>& values,
                                      std::string& error) {
    error.clear();
    values.clear();
    const auto auditFailure = [this](const std::string& detail) {
        if (logger_) logger_->audit("Direct register read failed: " + detail);
    };
    if (!initialized_) { error = "Application service is not initialized"; return false; }
    const AppConfig config = this->config();
    if (deviceAddress < 0x08 || deviceAddress > 0x77 || registerAddress > 0xFF ||
        expectedBytes < 1 || expectedBytes > 512) {
        error = "Device address must be 0x08..0x77, register address 0x00..0xFF, and expected bytes 1..512";
        auditFailure(error); return false;
    }
    AppLockGuard hardwareLock(hardwareMutex_);
    if (config.scanAdapterIndex >= adapterCount_) {
        error = "No CP2112 adapter is connected at the configured adapter index";
        auditFailure(error); return false;
    }
    DeviceHandle handle(api_);
    const Status status = api_.open(handle.address(), config.scanAdapterIndex, DEFAULT_VID, DEFAULT_PID);
    if (status != 0 || !handle.get()) {
        error = "Could not open adapter for direct read; status=" + std::to_string(status);
        auditFailure(error); return false;
    }
    Bus bus(api_, handle.get(), config.transferTimeoutMs, config.transferRetries);
    if (!bus.setClock(config.scanClockHz)) {
        error = "Failed to set bus clock for direct read";
        auditFailure(error); return false;
    }
    if (!bus.read(deviceAddress, registerAddress, 1, expectedBytes, values) || values.empty()) {
        signalGpioError(handle.get());
        if (logger_) logger_->audit("Direct register read i2c_error=" + transferErrorDescription(bus.lastDiagnostics()));
        error = "Read failed at device " + hex(deviceAddress) + ", register " + hex(registerAddress);
        auditFailure(error); return false;
    }
    if (logger_) logger_->audit("Direct register read device=" + hex(deviceAddress) +
                               " register=" + hex(registerAddress) +
                               " bytes=" + std::to_string(values.size()));
    return true;
}

// Compatibility wrapper for the CLI's existing one-byte register command.
bool ApplicationService::readRegister(unsigned deviceAddress, unsigned registerAddress,
                                      BYTE& value, std::string& error) {
    std::vector<BYTE> values;
    if (!readRegister(deviceAddress, registerAddress, 1, values, error)) return false;
    value = values.front();
    return true;
}

// Writes one byte to a register using the scan adapter and configured timeout.
bool ApplicationService::writeRegister(unsigned deviceAddress, unsigned registerAddress,
                                       BYTE value, std::string& error) {
    error.clear();
    const auto auditFailure = [this](const std::string& detail) {
        if (logger_) logger_->audit("Direct register write failed: " + detail);
    };
    if (!initialized_) { error = "Application service is not initialized"; return false; }
    const AppConfig config = this->config();
    if (deviceAddress < 0x08 || deviceAddress > 0x77 || registerAddress > 0xFF) {
        error = "Device address must be 0x08..0x77 and register address must be 0x00..0xFF";
        auditFailure(error); return false;
    }
    AppLockGuard hardwareLock(hardwareMutex_);
    if (config.scanAdapterIndex >= adapterCount_) {
        error = "No CP2112 adapter is connected at the configured adapter index";
        auditFailure(error); return false;
    }
    DeviceHandle handle(api_);
    const Status status = api_.open(handle.address(), config.scanAdapterIndex, DEFAULT_VID, DEFAULT_PID);
    if (status != 0 || !handle.get()) {
        error = "Could not open adapter for direct write; status=" + std::to_string(status);
        auditFailure(error); return false;
    }
    Bus bus(api_, handle.get(), config.transferTimeoutMs, config.transferRetries);
    if (!bus.setClock(config.scanClockHz)) {
        error = "Failed to set bus clock for direct write";
        auditFailure(error); return false;
    }
    if (!bus.writeReg(deviceAddress, static_cast<BYTE>(registerAddress), value)) {
        signalGpioError(handle.get());
        if (logger_) logger_->audit("Direct register write i2c_error=" + transferErrorDescription(bus.lastDiagnostics()));
        error = "Write failed at device " + hex(deviceAddress) + ", register " + hex(registerAddress);
        auditFailure(error); return false;
    }
    if (logger_) logger_->audit("Direct register write device=" + hex(deviceAddress) +
                               " register=" + hex(registerAddress) + " value=" + hex(value));
    return true;
}

// Streams driver readings until the caller's stop flag is set.
bool ApplicationService::streamDeviceStructured(size_t deviceIndex, std::atomic<bool>& stop,
                                                const StructuredStreamCallback& callback,
                                                std::string& error) {
    error.clear();
    DeviceConfig device;
    if (!getDevice(deviceIndex, device, error)) return false;
    std::unique_ptr<Driver> driver = makeDriver(device.type);
    if (!driver) {
        error = "No driver registered for " + device.type;
        if (logger_) logger_->audit("Driver-backed stream failed device=\"" + device.name +
                                    "\" type=" + device.type + " reason=\"" + error + "\"");
        return false;
    }
    AppLockGuard hardwareLock(hardwareMutex_);
    DeviceHandle handle(api_);
    if (!openDevice(device, *handle.address(), error)) {
        if (logger_) logger_->audit(driverAuditContext(device, *driver) +
                                   " operation=stream-start succeeded=false stage=open error=\"" + error + "\"");
        return false;
    }
    Bus bus(api_, handle.get(), config_.transferTimeoutMs, config_.transferRetries);
    if (!bus.setClock(device.clock)) {
        error = "Failed to set bus clock for " + device.name;
        if (logger_) logger_->audit(driverAuditContext(device, *driver) +
                                   " operation=stream-start succeeded=false stage=configure error=\"" + error + "\"");
        return false;
    }

    std::unique_ptr<std::ofstream> streamLog;
    if (device.logStream) {
        streamLog = logger_->openDeviceLog(device);
        if (!*streamLog) {
            logger_->audit(driverAuditContext(device, *driver) +
                           " operation=stream-log-open succeeded=false");
            streamLog.reset();
        }
    }
    logger_->audit(driverAuditContext(device, *driver) + " operation=stream-start interval=" +
                   std::to_string(device.refreshSeconds) + "s logging=" +
                   (device.logStream ? "on" : "off"));
    while (!stop.load()) {
        DeviceResult value;
        const bool readSucceeded = driver->readResult(bus, device, value);
        if (!readSucceeded) {
            signalGpioError(handle.get());
            if (logger_) logger_->audit(driverAuditContext(device, *driver) +
                                       " operation=stream i2c_error=" + transferErrorDescription(bus.lastDiagnostics()));
        }
        const std::string timestampValue = timestamp();
        callback(timestampValue, value);
        const std::string displayValue = formatDeviceResult(value);
        if (streamLog && *streamLog) {
            *streamLog << "\"" << timestampValue << "\",\"" << device.name
                       << "\",\"" << device.type << "\",\"" << hex(device.address)
                       << "\",\"" << displayValue << "\"\n";
            streamLog->flush();
        }
        for (unsigned i = 0; i < device.refreshSeconds * 10 && !stop.load(); ++i) Sleep(100);
    }
    logger_->audit(driverAuditContext(device, *driver) + " operation=stream-stop");
    return true;
}

// Preserves the original string callback for the CLI while using structured
// results internally and exposing them directly to future frontends.
bool ApplicationService::streamDevice(size_t deviceIndex, std::atomic<bool>& stop,
                                      const StreamCallback& callback, std::string& error) {
    return streamDeviceStructured(deviceIndex, stop,
        [&callback](const std::string& when, const DeviceResult& result) {
            callback(when, formatDeviceResult(result));
        }, error);
}

// Probes each configured I2C address with bounded transfers and optional progress reporting.
bool ApplicationService::scan(const ScanOptions& options, std::vector<ScanReading>& result,
                              std::string& error, const ScanProgressCallback& progress,
                              std::atomic<bool>* cancelRequested,
                              const ScanReadingCallback& readingProgress) {
    error.clear();
    if (!initialized_) { error = "Application service is not initialized"; return false; }
    AppLockGuard hardwareLock(hardwareMutex_);
    if (adapterCount_ == 0) {
        error = "No CP2112 adapters are connected; scan skipped.";
        return false;
    }
    if (options.adapterIndex >= adapterCount_) { error = "Scan adapter index is out of range"; return false; }
    if (options.startAddress < 0x08 || options.endAddress > 0x77 ||
        options.startAddress > options.endAddress || options.clockHz < 1 || options.clockHz > 400000 ||
        options.transferTimeoutMs < 1 || options.transferTimeoutMs > 1000 || options.transferRetries > 1000) {
        error = "Invalid scan address range or bus clock";
        return false;
    }
    DeviceHandle handle(api_);
    const Status status = api_.open(handle.address(), options.adapterIndex, DEFAULT_VID, DEFAULT_PID);
    if (status != 0 || !handle.get()) {
        error = "Could not open adapter for scan; status=" + std::to_string(status);
        return false;
    }
    Bus bus(api_, handle.get(), options.transferTimeoutMs, options.transferRetries, cancelRequested);
    if (!bus.setClock(options.clockHz)) { error = "Failed to set bus clock for scan"; return false; }

    if (logger_) {
        logger_->audit("Starting read-probe scan " + hex(options.startAddress) + "-" +
                       hex(options.endAddress) + " at " + std::to_string(options.clockHz) +
                       " Hz, timeout=" + std::to_string(options.transferTimeoutMs) +
                       " ms, retries=" + std::to_string(options.transferRetries));
    }
    result.clear();
    for (unsigned address = options.startAddress; address <= options.endAddress; ++address) {
        if (progress && !progress(address)) {
            error = "Scan cancelled by user.";
            if (logger_) logger_->audit("I2C scan cancelled by user");
            break;
        }
        std::vector<BYTE> data;
        ScanReading reading;
        reading.address = address;
        reading.readSucceeded = bus.read(address, 0x00, 1, 1, data, &reading.transfer);
        if (reading.readSucceeded) reading.value = data[0];
        result.push_back(reading);
        if (readingProgress && !readingProgress(reading)) {
            error = "Scan cancelled by user.";
            if (logger_) logger_->audit("I2C scan cancelled by user");
            break;
        }
        if (reading.transfer.cancelled) {
            error = "Scan cancelled by user.";
            if (logger_) logger_->audit("I2C scan cancelled during transfer");
            break;
        }
    }
    const size_t hits = static_cast<size_t>(std::count_if(
        result.begin(), result.end(), [](const ScanReading& reading) { return reading.readSucceeded; }));
    if (logger_) {
        logger_->audit("I2C scan complete; successful read probes=" + std::to_string(hits));
        for (const ScanReading& reading : result) {
            const TransferDiagnostics& transfer = reading.transfer;
            logger_->audit("I2C scan diagnostic address=" + hex(reading.address) +
                           " result=" + (reading.readSucceeded ? "found" : "not-found") +
                           " api_status=" + std::to_string(transfer.apiStatus) +
                           " state=" + std::to_string(transfer.transferState) +
                           " detail=" + std::to_string(transfer.detailedStatus) +
                           " retries=" + std::to_string(transfer.retries) +
                           " bytes=" + std::to_string(transfer.bytesTransferred) +
                           " timeout=" + (transfer.timedOut ? "true" : "false") +
                           " cancelled=" + (transfer.cancelled ? "true" : "false"));
            if (reading.readSucceeded) {
                logger_->audit("I2C scan found device address=" + hex(reading.address) +
                               " (" + std::to_string(reading.address) + ") value=" +
                               hex(reading.value));
            }
        }
    }
    return true;
}

// Applies temporary refresh and logging settings to one configured device.
bool ApplicationService::updateRuntimeDevice(size_t deviceIndex, unsigned refreshSeconds,
                                             bool logStream, std::string& error) {
    if (!updateDeviceSetting(deviceIndex, "refresh_seconds", static_cast<uint64_t>(refreshSeconds), error)) return false;
    return updateDeviceSetting(deviceIndex, "log_stream", logStream, error);
}

// Deletes old logs through the service logger and records the cleanup result.
bool ApplicationService::cleanLogs(unsigned days, LogCleanupResult& result, std::string& error) {
    error.clear();
    if (!initialized_ || !logger_) { error = "Application service is not initialized"; return false; }
    if (days == 0 || days > 36500) { error = "Cleanup days must be between 1 and 36500"; return false; }
    const AppConfig config = this->config();
    result = removeOldLogs(config.logDir, days, logger_->auditPath());
    logger_->audit("Manual log cleanup: deleted " + std::to_string(result.deleted) +
                   " log file(s) older than " + std::to_string(days) +
                   " day(s); failures=" + std::to_string(result.failed));
    return true;
}

// Deletes all timestamped audit and stream logs except the currently open audit log.
bool ApplicationService::clearAllLogs(LogCleanupResult& result, std::string& error) {
    error.clear();
    if (!initialized_ || !logger_) { error = "Application service is not initialized"; return false; }
    result = {};
    const AppConfig config = this->config();
    const std::string activeAudit = logger_->auditPath();
    const std::string pattern = config.logDir + "\\*";
    WIN32_FIND_DATAA entry{};
    HANDLE search = FindFirstFileA(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD status = GetLastError();
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return true;
        error = "Could not enumerate log directory (Windows error " + std::to_string(status) + ")";
        return false;
    }
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !isTimestampedLog(entry.cFileName)) continue;
        const std::string path = config.logDir + "\\" + entry.cFileName;
        if (lower(path) == lower(activeAudit)) continue;
        if (DeleteFileA(path.c_str())) ++result.deleted; else ++result.failed;
    } while (FindNextFileA(search, &entry));
    FindClose(search);
    logger_->audit("Manual log clear: deleted " + std::to_string(result.deleted) +
                   " log file(s); failures=" + std::to_string(result.failed));
    return true;
}

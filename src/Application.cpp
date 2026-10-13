#include "Application.h"
#include "DriverRegistry.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace {
std::string trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool parseUnsigned(const std::string& text, unsigned long& value) {
    try {
        size_t consumed = 0;
        value = std::stoul(text, &consumed, 0);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

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

bool loadConfig(const std::string& path, AppConfig& config, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "Cannot open config file: " + path;
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
            else if (key == "log_retention_days" && parseUnsigned(value, number) && number <= 36500)
                config.logRetentionDays = static_cast<unsigned>(number);
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
            device.type != "bq4050") {
            error = "Unsupported device type '" + device.type + "' for " + device.name;
            return false;
        }
    }
    return true;
}

std::string adapterString(Api& api, DWORD index, DWORD option) {
    char buffer[512]{};
    const Status status = api.getString(index, DEFAULT_VID, DEFAULT_PID, buffer, option);
    if (status != 0) return "(unavailable; API status " + std::to_string(status) + ")";
    return buffer;
}

bool isTimestampedLog(const std::string& name) {
    const std::string filename = lower(name);
    const bool audit = filename.size() == 6 + 15 + 4 &&
        filename.compare(0, 6, "audit_") == 0 &&
        filename.compare(filename.size() - 4, 4, ".log") == 0;
    const bool stream = filename.size() >= 1 + 1 + 15 + 4 &&
        filename.compare(filename.size() - 4, 4, ".csv") == 0;
    if (!audit && !stream) return false;
    const size_t timestampStart = audit ? 6 : filename.size() - 4 - 15 - 1;
    if (stream && filename[timestampStart] != '_') return false;
    const size_t digitsStart = audit ? timestampStart : timestampStart + 1;
    for (size_t i = digitsStart; i < digitsStart + 15; ++i) {
        if (i == digitsStart + 8) {
            if (filename[i] != '_') return false;
        } else if (!std::isdigit(static_cast<unsigned char>(filename[i]))) {
            return false;
        }
    }
    return true;
}

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

bool ApplicationService::initialize(const std::string& configPath, std::string& error) {
    error.clear();
    configPath_ = configPath;
    AppConfig loaded;
    if (!loadConfig(configPath_, loaded, error)) return false;
    config_ = std::move(loaded);
    logger_ = std::make_unique<Logger>(config_.logDir);
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

AppConfig ApplicationService::config() const {
    AppLockGuard lock(configMutex_);
    return config_;
}

bool ApplicationService::reload(std::string& error) {
    error.clear();
    AppConfig loaded;
    if (!loadConfig(configPath_, loaded, error)) {
        if (logger_) logger_->audit("Configuration reload failed: " + error);
        return false;
    }
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
        result.push_back(std::move(info));
    }
    return true;
}

bool ApplicationService::getDevice(size_t deviceIndex, DeviceConfig& device,
                                   std::string& error) const {
    if (!initialized_) { error = "Application service is not initialized"; return false; }
    AppLockGuard lock(configMutex_);
    if (deviceIndex >= config_.devices.size()) { error = "Device index is out of range"; return false; }
    device = config_.devices[deviceIndex];
    if (!device.enabled) { error = "Device is disabled in configuration"; return false; }
    return true;
}

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
    return true;
}

bool ApplicationService::readDevice(size_t deviceIndex, std::string& result, std::string& error) {
    error.clear();
    DeviceConfig device;
    if (!getDevice(deviceIndex, device, error)) return false;
    AppLockGuard hardwareLock(hardwareMutex_);
    DeviceHandle handle(api_);
    if (!openDevice(device, *handle.address(), error)) return false;
    Bus bus(api_, handle.get());
    if (!bus.setClock(device.clock)) {
        error = "Failed to set bus clock for " + device.name;
        return false;
    }
    std::unique_ptr<Driver> driver = makeDriver(device.type);
    if (!driver) { error = "No driver registered for " + device.type; return false; }
    result = driver->readStream(bus, device);
    if (logger_) logger_->audit(device.name + ": " + result);
    return true;
}

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
    return true;
}

bool ApplicationService::executeDeviceAction(size_t deviceIndex, const std::string& action,
                                             const std::vector<std::string>& parameters,
                                             std::string& result, std::string& error) {
    error.clear();
    result.clear();
    DeviceConfig device;
    if (!getDevice(deviceIndex, device, error)) return false;
    AppLockGuard hardwareLock(hardwareMutex_);
    DeviceHandle handle(api_);
    if (!openDevice(device, *handle.address(), error)) return false;
    Bus bus(api_, handle.get());
    if (!bus.setClock(device.clock)) {
        error = "Failed to set bus clock for " + device.name;
        return false;
    }
    std::unique_ptr<Driver> driver = makeDriver(device.type);
    if (!driver) { error = "No driver registered for " + device.type; return false; }
    const bool succeeded = driver->executeAction(bus, device, action, parameters, result);
    if (logger_) logger_->audit(device.name + ": " + result);
    return succeeded;
}

bool ApplicationService::streamDevice(size_t deviceIndex, std::atomic<bool>& stop,
                                      const StreamCallback& callback, std::string& error) {
    error.clear();
    DeviceConfig device;
    if (!getDevice(deviceIndex, device, error)) return false;
    AppLockGuard hardwareLock(hardwareMutex_);
    DeviceHandle handle(api_);
    if (!openDevice(device, *handle.address(), error)) return false;
    Bus bus(api_, handle.get());
    if (!bus.setClock(device.clock)) {
        error = "Failed to set bus clock for " + device.name;
        return false;
    }
    std::unique_ptr<Driver> driver = makeDriver(device.type);
    if (!driver) { error = "No driver registered for " + device.type; return false; }

    std::unique_ptr<std::ofstream> streamLog;
    if (device.logStream) {
        streamLog = logger_->openDeviceLog(device);
        if (!*streamLog) {
            logger_->audit("Could not open stream log for " + device.name);
            streamLog.reset();
        }
    }
    logger_->audit("Started stream for " + device.name + " interval=" +
                   std::to_string(device.refreshSeconds) + "s logging=" +
                   (device.logStream ? "on" : "off"));
    while (!stop.load()) {
        const std::string value = driver->readStream(bus, device);
        const std::string timestampValue = timestamp();
        callback(timestampValue, value);
        if (streamLog && *streamLog) {
            *streamLog << "\"" << timestampValue << "\",\"" << device.name
                       << "\",\"" << device.type << "\",\"" << hex(device.address)
                       << "\",\"" << value << "\"\n";
            streamLog->flush();
        }
        for (unsigned i = 0; i < device.refreshSeconds * 10 && !stop.load(); ++i) Sleep(100);
    }
    logger_->audit("Stopped stream for " + device.name);
    return true;
}

bool ApplicationService::scan(const ScanOptions& options, std::vector<ScanReading>& result,
                              std::string& error) {
    error.clear();
    if (!initialized_) { error = "Application service is not initialized"; return false; }
    AppLockGuard hardwareLock(hardwareMutex_);
    if (options.adapterIndex >= adapterCount_) { error = "Scan adapter index is out of range"; return false; }
    if (options.startAddress < 0x08 || options.endAddress > 0x77 ||
        options.startAddress > options.endAddress || options.clockHz < 1 || options.clockHz > 400000) {
        error = "Invalid scan address range or bus clock";
        return false;
    }
    DeviceHandle handle(api_);
    const Status status = api_.open(handle.address(), options.adapterIndex, DEFAULT_VID, DEFAULT_PID);
    if (status != 0 || !handle.get()) {
        error = "Could not open adapter for scan; status=" + std::to_string(status);
        return false;
    }
    Bus bus(api_, handle.get());
    if (!bus.setClock(options.clockHz)) { error = "Failed to set bus clock for scan"; return false; }

    if (logger_) {
        logger_->audit("Starting read-probe scan " + hex(options.startAddress) + "-" +
                       hex(options.endAddress) + " at " + std::to_string(options.clockHz) + " Hz");
    }
    result.clear();
    for (unsigned address = options.startAddress; address <= options.endAddress; ++address) {
        std::vector<BYTE> data;
        ScanReading reading;
        reading.address = address;
        reading.readSucceeded = bus.read(address, 0x00, 1, 1, data);
        if (reading.readSucceeded) reading.value = data[0];
        result.push_back(reading);
    }
    const size_t hits = static_cast<size_t>(std::count_if(
        result.begin(), result.end(), [](const ScanReading& reading) { return reading.readSucceeded; }));
    if (logger_) logger_->audit("I2C scan complete; successful read probes=" + std::to_string(hits));
    return true;
}

bool ApplicationService::updateRuntimeDevice(size_t deviceIndex, unsigned refreshSeconds,
                                             bool logStream, std::string& error) {
    error.clear();
    if (!initialized_) { error = "Application service is not initialized"; return false; }
    AppLockGuard lock(configMutex_);
    if (deviceIndex >= config_.devices.size()) { error = "Device index is out of range"; return false; }
    if (refreshSeconds < 1 || refreshSeconds > 3600) {
        error = "Refresh interval must be between 1 and 3600 seconds";
        return false;
    }
    DeviceConfig& device = config_.devices[deviceIndex];
    device.refreshSeconds = refreshSeconds;
    device.logStream = logStream;
    if (logger_) logger_->audit("Runtime settings changed for " + device.name);
    return true;
}

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

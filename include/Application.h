#pragma once
#include "AppTypes.h"
#include "CP2112Bus.h"
#include "DeviceDriver.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AppConfig {
    std::string bridgeDll = "SLABHIDtoSMBus.dll";
    std::string logDir = "logs";
    unsigned defaultRefreshSeconds = 2;
    bool defaultLogStream = true;
    unsigned scanAdapterIndex = 0;
    unsigned scanStartAddress = 0x08;
    unsigned scanEndAddress = 0x77;
    unsigned scanClockHz = 100000;
    unsigned logRetentionDays = 30;
    std::vector<DeviceConfig> devices;
};

struct AdapterInformation {
    unsigned index = 0;
    std::string product;
    std::string manufacturer;
    std::string serial;
    std::string devicePath;
    bool attributesAvailable = false;
    WORD vid = 0;
    WORD pid = 0;
    WORD release = 0;
    Status attributeStatus = 0;
};

struct ScanOptions {
    unsigned adapterIndex = 0;
    unsigned startAddress = 0x08;
    unsigned endAddress = 0x77;
    unsigned clockHz = 100000;
};

struct ScanReading {
    unsigned address = 0;
    bool readSucceeded = false;
    BYTE value = 0;
};

struct DriverInformation {
    std::string title;
    std::string capabilities;
    std::vector<DriverAction> actions;
};

struct LogCleanupResult {
    size_t deleted = 0;
    size_t failed = 0;
};

class ApplicationService {
public:
    using StreamCallback = std::function<void(const std::string&, const std::string&)>;

    ApplicationService();
    ~ApplicationService();
    ApplicationService(const ApplicationService&) = delete;
    ApplicationService& operator=(const ApplicationService&) = delete;

    bool initialize(const std::string& configPath, std::string& error);
    AppConfig config() const;
    bool reload(std::string& error);
    bool adapters(std::vector<AdapterInformation>& result, std::string& error);
    bool driverInformation(size_t deviceIndex, DriverInformation& result,
                           std::string& error) const;
    bool readDevice(size_t deviceIndex, std::string& result, std::string& error);
    bool executeDeviceAction(size_t deviceIndex, const std::string& action,
                             const std::vector<std::string>& parameters,
                             std::string& result, std::string& error);
    bool streamDevice(size_t deviceIndex, std::atomic<bool>& stop,
                      const StreamCallback& callback, std::string& error);
    bool scan(const ScanOptions& options, std::vector<ScanReading>& result,
              std::string& error);
    bool updateRuntimeDevice(size_t deviceIndex, unsigned refreshSeconds,
                             bool logStream, std::string& error);
    bool cleanLogs(unsigned days, LogCleanupResult& result, std::string& error);

private:
    bool getDevice(size_t deviceIndex, DeviceConfig& device,
                   std::string& error) const;
    bool openDevice(const DeviceConfig& config, Device& handle,
                    std::string& error);

    std::string configPath_;
    AppConfig config_;
    std::unique_ptr<Logger> logger_;
    Api api_;
    DWORD adapterCount_ = 0;
    bool initialized_ = false;
    mutable AppMutex configMutex_;
    AppMutex hardwareMutex_;
};

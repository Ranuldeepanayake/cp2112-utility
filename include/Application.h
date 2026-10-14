// Declares the UI-independent application service used by the console frontend.
#pragma once
#include "AppTypes.h"
#include "CP2112Bus.h"
#include "DeviceDriver.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using SettingValue = std::variant<uint64_t, bool, std::string>;

struct AppConfig {
    std::string bridgeDll = "SLABHIDtoSMBus.dll";
    std::string logDir = "logs";
    std::string buildTimestamp = "000000000000";
    std::string buildNumber = "0";
    bool betaBuild = false;
    unsigned defaultRefreshSeconds = 2;
    bool defaultLogStream = true;
    unsigned scanAdapterIndex = 0;
    unsigned scanStartAddress = 0x08;
    unsigned scanEndAddress = 0x77;
    unsigned scanClockHz = 100000;
    unsigned transferTimeoutMs = 500;
    unsigned transferRetries = 0;
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
    bool partNumberAvailable = false;
    BYTE partNumber = 0;
    BYTE partVersion = 0;
    Status partNumberStatus = 0;
    bool usbConfigAvailable = false;
    WORD configuredVid = 0;
    WORD configuredPid = 0;
    BYTE configuredPower = 0;
    BYTE configuredPowerMode = 0;
    WORD configuredRelease = 0;
    Status usbConfigStatus = 0;
    bool lockAvailable = false;
    BYTE lockBits = 0;
    Status lockStatus = 0;
    bool smbusConfigAvailable = false;
    DWORD smbusBitRate = 0;
    BYTE smbusAddress = 0;
    BOOL smbusAutoReadRespond = FALSE;
    WORD smbusWriteTimeout = 0;
    WORD smbusReadTimeout = 0;
    BOOL smbusSclLowTimeout = FALSE;
    WORD smbusTransferRetries = 0;
    Status smbusConfigStatus = 0;
    bool smbusLibraryVersionAvailable = false;
    BYTE smbusLibraryMajor = 0;
    BYTE smbusLibraryMinor = 0;
    BOOL smbusLibraryRelease = FALSE;
    Status smbusLibraryVersionStatus = 0;
    bool hidLibraryVersionAvailable = false;
    BYTE hidLibraryMajor = 0;
    BYTE hidLibraryMinor = 0;
    BOOL hidLibraryRelease = FALSE;
    Status hidLibraryVersionStatus = 0;
};

struct ScanOptions {
    unsigned adapterIndex = 0;
    unsigned startAddress = 0x08;
    unsigned endAddress = 0x77;
    unsigned clockHz = 100000;
    unsigned transferTimeoutMs = 500;
    unsigned transferRetries = 0;
};

struct ScanReading {
    unsigned address = 0;
    bool readSucceeded = false;
    BYTE value = 0;
    TransferDiagnostics transfer;
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
    using StructuredStreamCallback = std::function<void(const std::string&, const DeviceResult&)>;
    // Return false from the callback to cancel the scan before the next probe.
    using ScanProgressCallback = std::function<bool(unsigned)>;

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
    bool readDeviceResult(size_t deviceIndex, DeviceResult& result, std::string& error);
    bool readDevice(size_t deviceIndex, std::string& result, std::string& error);
    bool executeDeviceAction(size_t deviceIndex, const std::string& action,
                             const std::vector<std::string>& parameters,
                             std::string& result, std::string& error);
    bool readRegister(unsigned deviceAddress, unsigned registerAddress,
                      BYTE& value, std::string& error);
    bool writeRegister(unsigned deviceAddress, unsigned registerAddress,
                       BYTE value, std::string& error);
    bool streamDevice(size_t deviceIndex, std::atomic<bool>& stop,
                      const StreamCallback& callback, std::string& error);
    bool streamDeviceStructured(size_t deviceIndex, std::atomic<bool>& stop,
                                const StructuredStreamCallback& callback, std::string& error);
    bool updateGeneralSetting(const std::string& key, const SettingValue& value,
                              std::string& error);
    bool updateDeviceSetting(size_t deviceIndex, const std::string& key,
                             const SettingValue& value, std::string& error);
    bool scan(const ScanOptions& options, std::vector<ScanReading>& result,
              std::string& error, const ScanProgressCallback& progress = {},
              std::atomic<bool>* cancelRequested = nullptr);
    bool updateRuntimeDevice(size_t deviceIndex, unsigned refreshSeconds,
                             bool logStream, std::string& error);
    bool cleanLogs(unsigned days, LogCleanupResult& result, std::string& error);

private:
    bool getDevice(size_t deviceIndex, DeviceConfig& device,
                   std::string& error) const;
    bool openDevice(const DeviceConfig& config, Device& handle,
                    std::string& error);
    bool persistSetting(const std::string& section, const std::string& key,
                        const std::string& value, std::string& error) const;

    std::string configPath_;
    AppConfig config_;
    std::unique_ptr<Logger> logger_;
    Api api_;
    DWORD adapterCount_ = 0;
    bool initialized_ = false;
    mutable AppMutex configMutex_;
    AppMutex hardwareMutex_;
};

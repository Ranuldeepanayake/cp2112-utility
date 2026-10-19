// Defines shared configuration, device, synchronization, timestamp, and logging types.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

struct DeviceConfig {
    std::string name;
    std::string type;
    unsigned address = 0;
    DWORD clock = 100000;
    unsigned refreshSeconds = 2;
    bool logStream = true;
    unsigned adapterIndex = 0;
    bool enabled = true;
};

class AppMutex {
public:
    AppMutex() { InitializeCriticalSection(&section_); }
    ~AppMutex() { DeleteCriticalSection(&section_); }
    void lock() { EnterCriticalSection(&section_); }
    void unlock() { LeaveCriticalSection(&section_); }
private:
    CRITICAL_SECTION section_;
    AppMutex(const AppMutex&) = delete;
    AppMutex& operator=(const AppMutex&) = delete;
};

class AppLockGuard {
public:
    explicit AppLockGuard(AppMutex& mutex) : mutex_(mutex) { mutex_.lock(); }
    ~AppLockGuard() { mutex_.unlock(); }
private:
    AppMutex& mutex_;
    AppLockGuard(const AppLockGuard&) = delete;
    AppLockGuard& operator=(const AppLockGuard&) = delete;
};

inline std::string timestamp(bool fileSafe = false) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_MSC_VER)
    localtime_s(&tm, &t);
#elif defined(_WIN32)
    static AppMutex localTimeMutex;
    AppLockGuard lock(localTimeMutex);
    const std::tm* local = std::localtime(&t);
    if (!local) throw std::runtime_error("Could not convert local time");
    tm = *local;
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream o;
    o << std::put_time(&tm, fileSafe ? "%Y%m%d_%H%M%S" : "%Y-%m-%d %H:%M:%S");
    return o.str();
}
inline std::string hex(unsigned v, int width = 2) {
    std::ostringstream o;
    o << "0x" << std::uppercase << std::hex << std::setw(width) << std::setfill('0') << v;
    return o.str();
}
inline unsigned bcdToDec(BYTE b) {
    return ((b >> 4) * 10) + (b & 0x0f);
}
inline BYTE decToBcd(unsigned n) {
    return static_cast<BYTE>(((n / 10) << 4) | (n % 10));
}
class Logger {
    std::string dir_;
    std::string frontendTag_;
    std::string auditPath_;
    std::ofstream audit_;
    AppMutex auditMutex_;
public:
    explicit Logger(std::string dir, std::string frontendTag = "app")
        : dir_(std::move(dir)), frontendTag_(std::move(frontendTag)) {
        CreateDirectoryA(dir_.c_str(), nullptr);
        auditPath_ = dir_ + "\\audit_" + frontendTag_ + "_" + timestamp(true) + ".log";
        audit_.open(auditPath_, std::ios::app);
    }
    bool available() const { return audit_.is_open(); }
    const std::string& auditPath() const { return auditPath_; }
    void audit(const std::string& message) {
        AppLockGuard lock(auditMutex_);
        const std::string line = "[" + timestamp() + "] " + message;
        if (audit_) { audit_ << line << "\n"; audit_.flush(); }
    }
    std::unique_ptr<std::ofstream> openDeviceLog(const DeviceConfig& d) {
        std::string safe = d.name;
        for (char& c : safe) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
        auto f = std::make_unique<std::ofstream>(dir_ + "\\" + safe + "_" + frontendTag_ + "_" + timestamp(true) + ".csv", std::ios::app);
        if (*f) *f << "timestamp,device,type,address,stream\n";
        return f;
    }
};

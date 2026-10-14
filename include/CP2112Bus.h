// Declares the dynamically loaded CP2112 API wrapper and reusable I2C bus operations.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <string>
#include <vector>

constexpr WORD DEFAULT_VID = 0x10C4;
constexpr WORD DEFAULT_PID = 0xEA90;

using Device = void*;
using Status = int;

using GetNumDevicesFn = Status (WINAPI*)(DWORD*, WORD, WORD);
using GetStringFn = Status (WINAPI*)(DWORD, WORD, WORD, char*, DWORD);
using GetAttributesFn = Status (WINAPI*)(DWORD, WORD, WORD, WORD*, WORD*, WORD*);
using OpenFn = Status (WINAPI*)(Device*, DWORD, WORD, WORD);
using CloseFn = Status (WINAPI*)(Device);
using SetConfigFn = Status (WINAPI*)(Device, DWORD, BYTE, BOOL, WORD, WORD, BOOL, WORD);
using ReadRequestFn = Status (WINAPI*)(Device, BYTE, WORD);
using AddressReadFn = Status (WINAPI*)(Device, BYTE, WORD, BYTE, BYTE*);
using GetReadResponseFn = Status (WINAPI*)(Device, int*, BYTE*, BYTE, BYTE*);
using WriteRequestFn = Status (WINAPI*)(Device, BYTE, BYTE*, BYTE);
using TransferStatusRequestFn = Status (WINAPI*)(Device);
using GetTransferStatusResponseFn = Status (WINAPI*)(Device, int*, int*, WORD*, WORD*);
using GetPartNumberFn = Status (WINAPI*)(Device, BYTE*, BYTE*);
using GetLibraryVersionFn = Status (WINAPI*)(BYTE*, BYTE*, BOOL*);

constexpr int TRANSFER_IDLE = 0;
constexpr int TRANSFER_BUSY = 1;
constexpr int TRANSFER_COMPLETE = 2;
constexpr int TRANSFER_ERROR = 3;

struct Api {
    HMODULE dll = nullptr;
    GetNumDevicesFn getNumDevices = nullptr;
    GetStringFn getString = nullptr;
    GetAttributesFn getAttributes = nullptr;
    OpenFn open = nullptr;
    CloseFn close = nullptr;
    SetConfigFn setConfig = nullptr;
    ReadRequestFn readRequest = nullptr;
    AddressReadFn addressRead = nullptr;
    GetReadResponseFn getReadResponse = nullptr;
    WriteRequestFn writeRequest = nullptr;
    TransferStatusRequestFn transferStatusRequest = nullptr;
    GetTransferStatusResponseFn getTransferStatusResponse = nullptr;
    GetPartNumberFn getPartNumber = nullptr;
    GetLibraryVersionFn getLibraryVersion = nullptr;

    template<class T> bool load(T& fn, const char* name, std::string& error) {
        // GetProcAddress exposes a generic FARPROC. Windows guarantees that
        // the exported function address can be used with its declared API
        // signature, but GCC warns about a direct function-pointer cast.
        const FARPROC symbol = GetProcAddress(dll, name);
        if (!symbol) {
            fn = nullptr;
            error = std::string("Missing API export: ") + name;
            return false;
        }

        // Copy the pointer representation across the Windows loader boundary
        // without an incompatible function-pointer cast. The size assertion
        // prevents this wrapper from being used with an incompatible pointer
        // representation on a different target architecture.
        static_assert(sizeof(T) == sizeof(FARPROC),
                      "Loaded API function pointer size does not match FARPROC");
        std::memcpy(&fn, &symbol, sizeof(fn));
        return true;
    }
    bool init(const std::string& dllPath, std::string& error) {
        dll = LoadLibraryA(dllPath.c_str());
        if (!dll) {
            error = "Cannot load " + dllPath + " (Windows error " +
                    std::to_string(GetLastError()) + ")";
            return false;
        }
        return load(getNumDevices, "HidSmbus_GetNumDevices", error) &&
               load(getString, "HidSmbus_GetString", error) &&
               load(getAttributes, "HidSmbus_GetAttributes", error) &&
               load(open, "HidSmbus_Open", error) &&
               load(close, "HidSmbus_Close", error) &&
               load(setConfig, "HidSmbus_SetSmbusConfig", error) &&
               load(readRequest, "HidSmbus_ReadRequest", error) &&
               load(addressRead, "HidSmbus_AddressReadRequest", error) &&
               load(getReadResponse, "HidSmbus_GetReadResponse", error) &&
               load(writeRequest, "HidSmbus_WriteRequest", error) &&
               load(transferStatusRequest, "HidSmbus_TransferStatusRequest", error) &&
               load(getTransferStatusResponse, "HidSmbus_GetTransferStatusResponse", error) &&
               load(getPartNumber, "HidSmbus_GetPartNumber", error) &&
               load(getLibraryVersion, "HidSmbus_GetLibraryVersion", error);
    }
    ~Api() { if (dll) FreeLibrary(dll); }
};

class Bus {
    Api& api_;
    Device handle_ = nullptr;
    DWORD timeoutMs_ = 500;
    WORD transferRetries_ = 0;
public:
    Bus(Api& api, Device h, unsigned timeoutMs = 500, unsigned transferRetries = 0)
        : api_(api), handle_(h), timeoutMs_(timeoutMs),
          transferRetries_(static_cast<WORD>(std::min<unsigned>(transferRetries, 1000))) {}
    bool setClock(DWORD hz) {
        // The CP2112 enforces its own transfer timeout. Keep it aligned with
        // the application's timeout instead of relying only on host polling.
        // The DLL accepts a maximum of 1000 ms; values above that are capped.
        const WORD hardwareTimeout = static_cast<WORD>(std::min<DWORD>(timeoutMs_, 1000));
        return api_.setConfig(handle_, hz, 0x02, TRUE, hardwareTimeout, hardwareTimeout,
                              TRUE, transferRetries_) == 0;
    }
    bool read(unsigned address, unsigned reg, unsigned regBytes, unsigned length, std::vector<BYTE>& out) {
        if (length < 1 || length > 512 || regBytes < 1 || regBytes > 2) return false;
        BYTE target[2] = { static_cast<BYTE>(regBytes == 2 ? (reg >> 8) : reg), static_cast<BYTE>(reg & 0xff) };
        BYTE* p = regBytes == 1 ? &target[1] : target;
        if (api_.addressRead(handle_, static_cast<BYTE>(address << 1), static_cast<WORD>(length),
                             static_cast<BYTE>(regBytes), p) != 0) return false;
        out.clear();
        const DWORD start = GetTickCount();
        while (GetTickCount() - start < timeoutMs_) {
            BYTE buf[61]{}, got = 0;
            int state = TRANSFER_IDLE;
            if (api_.getReadResponse(handle_, &state, buf, sizeof(buf), &got) != 0) return false;
            if (state == TRANSFER_ERROR) return false;
            if (got) {
                if (out.size() + got > length) return false;
                out.insert(out.end(), buf, buf + got);
            }
            if (state == TRANSFER_COMPLETE) return out.size() == length;
            if (state != TRANSFER_BUSY && state != TRANSFER_IDLE) return false;
            Sleep(1);
        }
        return false;
    }
    bool rawRead(unsigned address, unsigned length, std::vector<BYTE>& out) {
        if (length < 1 || length > 512) return false;
        if (api_.readRequest(handle_, static_cast<BYTE>(address << 1), static_cast<WORD>(length)) != 0) return false;
        out.clear();
        const DWORD start = GetTickCount();
        while (GetTickCount() - start < timeoutMs_) {
            BYTE buf[61]{}, got = 0; int state = TRANSFER_IDLE;
            if (api_.getReadResponse(handle_, &state, buf, sizeof(buf), &got) != 0) return false;
            if (state == TRANSFER_ERROR) return false;
            if (got) {
                if (out.size() + got > length) return false;
                out.insert(out.end(), buf, buf + got);
            }
            if (state == TRANSFER_COMPLETE) return out.size() == length;
            if (state != TRANSFER_BUSY && state != TRANSFER_IDLE) return false;
            Sleep(1);
        }
        return false;
    }
    bool writeBytes(unsigned address, const std::vector<BYTE>& bytes) {
        if (bytes.empty() || bytes.size() > 61) return false;
        auto copy = bytes;
        if (api_.writeRequest(handle_, static_cast<BYTE>(address << 1), copy.data(), static_cast<BYTE>(copy.size())) != 0) return false;
        const DWORD start = GetTickCount();
        while (GetTickCount() - start < timeoutMs_) {
            if (api_.transferStatusRequest(handle_) != 0) return false;
            int state = TRANSFER_IDLE, detail = 0; WORD retries = 0, transferred = 0;
            if (api_.getTransferStatusResponse(handle_, &state, &detail, &retries, &transferred) != 0) return false;
            if (state == TRANSFER_COMPLETE) return true;
            if (state == TRANSFER_ERROR) return false;
            if (state != TRANSFER_BUSY && state != TRANSFER_IDLE) return false;
            Sleep(2);
        }
        return false;
    }
    bool writeReg(unsigned address, BYTE reg, BYTE value) { return writeBytes(address, {reg, value}); }
    bool writeCommand(unsigned address, BYTE value) { return writeBytes(address, {value}); }
};

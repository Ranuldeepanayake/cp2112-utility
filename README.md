# CP2112 Device Console

A modular Windows command-line application for communicating with I²C/SMBus devices through the Silicon Labs CP2112 USB-to-SMBus bridge.

The application separates the CLI and configuration handling from the device-specific drivers. Device drivers are individual C++ source files under `drivers/`, compiled into the application at build time. At runtime, `config/devices.ini` selects which registered driver, address, adapter, bus speed, refresh interval, and stream-logging setting to use.

> **Project status:** This is a starter implementation. The source package has not been verified against every Silicon Labs SDK version or tested with all target devices. Compile it with your installed SDK/toolchain and validate each operation on your hardware before relying on it. Register writes can change device state.

## Contents

- [Features](#features)
- [Architecture and driver separation](#architecture-and-driver-separation)
- [Directory structure](#directory-structure)
- [Prerequisites](#prerequisites)
- [Obtain the Silicon Labs SDK files](#obtain-the-silicon-labs-sdk-files)
- [Build with MinGW-w64](#build-with-mingw-w64)
- [Run the application](#run-the-application)
- [Main menu and navigation](#main-menu-and-navigation)
- [Configuration reference](#configuration-reference)
- [Supported devices and operations](#supported-devices-and-operations)
- [Data streaming and logs](#data-streaming-and-logs)
- [Adding another device driver](#adding-another-device-driver)
- [Hardware and I²C notes](#hardware-and-i²c-notes)
- [Troubleshooting](#troubleshooting)
- [Known limitations](#known-limitations)
- [References](#references)

## Features

- Enumerates CP2112 adapters and displays adapter information.
- Lists enabled device definitions from a text configuration file.
- Provides a CLI menu for selecting a configured device.
- Uses device-specific driver functions for supported reads and configuration writes.
- Supports one-shot reads and periodic data streams.
- Allows a stream refresh interval to be changed for the current run.
- Allows stream logging to be enabled or disabled for the current run.
- Creates timestamped CSV files for device data streams.
- Creates a separate timestamped audit log for application and user actions.
- Includes an interactive I²C read-probe scan.
- Keeps the main application separate from device-specific source files.
- Routes configuration, adapter access, device operations, scanning, streaming, and log cleanup through a UI-independent application service.

## Architecture and driver separation

### Main application

`src/main.cpp` is the CLI frontend. It gathers user input and displays results, but does not load the CP2112 DLL or perform device I/O directly.

### Application service

`include/Application.h` and `src/Application.cpp` provide the UI-independent `ApplicationService`. It owns configuration loading/reloading, adapter enumeration and opening, device reads and actions, streaming callbacks, I²C scans, runtime device settings, and log cleanup. A future frontend can call these operations without using console menus or relying on `std::cin`/`std::cout`.

### Device drivers

Each driver is a separate C++ translation unit:

- `drivers/BMP280.cpp`
- `drivers/MPU6050.cpp`
- `drivers/BH1750.cpp`
- `drivers/DS3231.cpp`
- `drivers/BQ4050.cpp`

Each driver implements the common interface declared in `include/DeviceDriver.h`. Drivers expose metadata and callable actions, with arguments and confirmation prompts described as data; they contain no CLI input or output. The interface includes:

- `title()` — human-readable driver title.
- `capabilities()` — short description of supported operations.
- `readStream(...)` — obtains a device reading formatted as text.
- `actions()` — describes available device-specific operations, required parameters, and any confirmation prompt.
- `executeAction(...)` — executes an action from explicit parameter values and returns a result without UI interaction.

### Driver registry

`src/DriverRegistry.cpp` maps the `type=` value in the configuration file to a compiled-in driver factory. `include/DriverRegistry.h` declares the factory functions.

### Shared CP2112 layer

`include/CP2112Bus.h` declares the dynamically loaded Silicon Labs API and provides reusable bus operations, including register reads, raw reads, command writes, and register writes.

### Important compile-time distinction

C++ headers are included and compiled at build time. A text file cannot tell an already-built executable to compile or include an arbitrary header at runtime. In this project, the configuration selects one of the drivers compiled into the executable.

To load third-party drivers without rebuilding the main program, the design would need a separate plugin ABI and drivers compiled as DLLs. That is not the current design.

## Directory structure

```text
cp2112-device-console/
├── src/
│   ├── main.cpp
│   ├── Application.cpp
│   └── DriverRegistry.cpp
├── drivers/
│   ├── BMP280.cpp
│   ├── MPU6050.cpp
│   ├── BH1750.cpp
│   ├── DS3231.cpp
│   └── BQ4050.cpp
├── include/
│   ├── Application.h
│   ├── AppTypes.h
│   ├── CP2112Bus.h
│   ├── DeviceDriver.h
│   └── DriverRegistry.h
├── config/
│   └── devices.ini
├── bin/
│   ├── cp2112_console.exe        # generated during build
│   ├── SLABHIDtoSMBus.dll        # supplied by Silicon Labs SDK
│   └── SLABHIDDevice.dll         # supplied by Silicon Labs SDK
├── logs/                         # generated at runtime
└── README.md
```

Keep the project directory structure intact unless you also update the relevant paths in the configuration file and build command.

## Prerequisites

### Software

- Windows 10 or Windows 11.
- MinGW-w64 GCC/G++ with C++17 support.
- Silicon Labs CP2112 SDK/runtime files.
- The following DLLs from a compatible Silicon Labs SDK:
  - `SLABHIDtoSMBus.dll`
  - `SLABHIDDevice.dll`

Check the compiler:

```powershell
g++ --version
```

If PowerShell reports that `g++` is not recognized, install MinGW-w64 and add its `bin` directory to the Windows `PATH`.

### Hardware

- A CP2112 adapter.
- One or more supported I²C devices.
- Correct SDA, SCL, and ground wiring.
- Compatible logic-voltage levels and suitable I²C pull-up resistors.
- A USB cable and port that work with the adapter.

Windows normally supplies the USB HID class driver used by CP2112. The application still requires the Silicon Labs user-space API DLLs.

## Obtain the Silicon Labs SDK files

Start at the official [Silicon Labs CP2112 software page](https://www.silabs.com/interface/usb-bridges/classic/device.cp2112?tab=softwareandtools).

Download/install the applicable CP2112 software package and locate the runtime DLLs. Copy these files into the project's `bin/` directory:

```text
bin/
├── SLABHIDtoSMBus.dll
└── SLABHIDDevice.dll
```

The DLLs must match the architecture of the executable: use 64-bit DLLs for a 64-bit build, or 32-bit DLLs for a 32-bit build. Keep the DLL versions compatible with one another and with the SDK.

The source dynamically loads `SLABHIDtoSMBus.dll`. It does not require an MSVC import library for this dynamic-loading approach.

## Build with MinGW-w64

Run these commands from the **project root** (the directory containing `src/`, `drivers/`, `include/`, and `config/`).

### 1. Create the output directory if needed

```powershell
New-Item -ItemType Directory -Force bin, logs
```

### 2. Compile and link the main application and all drivers

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude src\main.cpp src\Application.cpp src\DriverRegistry.cpp drivers\BMP280.cpp drivers\MPU6050.cpp `
drivers\BH1750.cpp drivers\DS3231.cpp drivers\BQ4050.cpp -o bin\cp2112_console.exe
```

The build command compiles each `.cpp` file and links them into one executable. The device drivers are logically separate source modules, but they are not separately loaded DLL plugins.

If you add a driver, include its source file in this command.

### 3. Verify the output

```powershell
Get-ChildItem bin
```

You should see `cp2112_console.exe` and the two Silicon Labs DLLs. The executable is created by compilation; the DLLs must be copied from the SDK.

## Run the application

From the project root:

```powershell
.\bin\cp2112_console.exe
```

By default, the program reads `config\devices.ini`.

To use a different configuration file:

```powershell
.\bin\cp2112_console.exe .\config\devices.ini
```

Use paths relative to the working directory from which you launch the application unless you update the source to resolve paths relative to the executable.

## Main menu and navigation

The main menu provides these options:

1. **List configured devices** — display enabled device entries and their type, address, clock, refresh interval, and logging setting.
2. **Select device** — choose a configured device and enter its device menu.
3. **Adapter information** — enumerate the CP2112 adapters detected by the application.
4. **Reload configuration** — reread device definitions and scan defaults from the configuration file. Changes to `bridge_dll` or `log_dir` still require restarting.
5. **Scan I²C addresses** — scan the configured address range and bus clock, or enter overrides. Press Enter at a scan prompt to accept its configured default.
6. **Clean old logs** — interactively delete application logs older than a specified number of days.
0. **Exit** — close the application.

In a device menu:

1. **Read once** — read the current data stream once.
2. **Start data stream** — repeatedly read at the configured interval; press Enter to stop.
3. **Device configuration/functions** — open the selected driver's device-specific menu.
4. **Change refresh interval** — set a new interval for the current run.
5. **Toggle stream logging** — enable or disable data logging for the current run.
0. **Back to device list** — return to the main device list.

Runtime changes to refresh interval and stream logging are not saved back to `devices.ini`. Edit the configuration file and select **Reload configuration** to apply device definitions, including enabled/disabled status, and scan defaults. Reloading replaces runtime device edits with the values from the file. Changes to `bridge_dll` and `log_dir` require restarting.

## Configuration reference

The default configuration is `config/devices.ini`. It uses an INI-like format with a `[general]` section and one `[device NAME]` section per configured device.

Example:

```ini
[general]
bridge_dll=bin\SLABHIDtoSMBus.dll
log_dir=logs
default_refresh_seconds=2
default_log_stream=true
scan_adapter_index=0
scan_start_address=0x08
scan_end_address=0x77
scan_clock_hz=100000
log_retention_days=30

[device BMP280-1]
type=bmp280
address=0x76
clock_hz=100000
adapter_index=0
refresh_seconds=2
log_stream=true
enabled=true

[device MPU6050-1]
type=mpu6050
address=0x68
clock_hz=100000
adapter_index=0
refresh_seconds=1
log_stream=true
enabled=false

[device BH1750-1]
type=bh1750
address=0x23
clock_hz=100000
adapter_index=0
refresh_seconds=2
log_stream=true
enabled=true

[device DS3231-1]
type=ds3231
address=0x68
clock_hz=100000
adapter_index=0
refresh_seconds=5
log_stream=true
enabled=true

[device BQ4050-1]
type=bq4050
address=0x0B
clock_hz=100000
adapter_index=0
refresh_seconds=5
log_stream=true
enabled=false
```

### General settings

| Key | Meaning | Accepted values / default |
|---|---|---|
| `bridge_dll` | Path to Silicon Labs HID-to-SMBus DLL | Default: `bin\SLABHIDtoSMBus.dll` |
| `log_dir` | Directory for audit and stream logs | Default: `logs` |
| `default_refresh_seconds` | Default interval applied to device entries that do not override it | 1–3600; default `2` |
| `default_log_stream` | Default stream logging choice for device entries | `true` or `false`; default `true` |
| `scan_adapter_index` | Default CP2112 adapter used for scans; can be overridden at the scan prompt | 0–255; default `0` |
| `scan_start_address` | Default first 7-bit I²C address for scans | `0x08`–`0x77`; default `0x08` |
| `scan_end_address` | Default last 7-bit I²C address for scans | `0x08`–`0x77`, must be at least the start address; default `0x77` |
| `scan_clock_hz` | Default scan bus speed | 1–400000 Hz; default `100000` |
| `log_retention_days` | Automatically delete timestamped application log files at startup when their last-modified time is at least this many days old; oldest files are deleted first. `0` disables automatic cleanup. | 0–36500; default `30` |

### Device settings

| Key | Meaning | Accepted values / default |
|---|---|---|
| Section name | Friendly name used in menus and log filenames | Example: `[device BMP280-1]` |
| `type` | Selects the compiled-in driver | `bmp280`, `mpu6050`, `bh1750`, `ds3231`, `bq4050` |
| `address` | 7-bit I²C target address | Decimal or hexadecimal, e.g. `104` or `0x68`; normally `0x08`–`0x77` |
| `clock_hz` | Requested bus clock | 1–400000 Hz; default `100000` |
| `adapter_index` | CP2112 adapter index | Usually `0` when one adapter is attached |
| `refresh_seconds` | Data stream interval | 1–3600 seconds |
| `log_stream` | Whether the stream is written to a CSV file | `true` or `false` |
| `enabled` | Whether the device appears in the selectable list | `true` or `false` |

Boolean values accept `true`/`false`, `yes`/`no`, `on`/`off`, and `1`/`0`. Numeric values may be decimal or `0x`-prefixed hexadecimal.

The configuration parser rejects unknown or malformed settings. If configuration loading fails, read the error message and correct the named line or setting.

### Common device addresses

These are common defaults; the actual address depends on the part and board straps:

| Device | Common address(es) |
|---|---|
| BMP280 | `0x76` or `0x77` |
| MPU-6050 | `0x68` or `0x69` |
| BH1750 | `0x23` or `0x5C` |
| DS3231 | `0x68` |
| TI BQ4050 | `0x0B` (default Smart Battery address) |

**Address conflict:** MPU-6050 and DS3231 commonly both use `0x68`. Two devices at the same address cannot normally share the same bus. Use a different valid address strap where supported, a separate adapter/bus, or an I²C multiplexer. The sample configuration disables the MPU-6050 for this reason. If AD0 is high, the MPU-6050 commonly responds at `0x69`.

## Supported devices and operations

The exact available actions are shown in each device's CLI submenu.

### BMP280

- Reads the chip ID register (`0xD0`).
- Reads raw pressure and temperature ADC values starting at `0xF7`.
- Allows writing the `ctrl_meas` register (`0xF4`).
- Allows writing the `config` register (`0xF5`).

The current implementation displays raw ADC values. It does not yet read all calibration coefficients or compensate raw values into temperature in °C and pressure in Pa/hPa.

### MPU-6050

- Reads `WHO_AM_I` (`0x75`).
- Reads 14 bytes starting at `0x3B`, covering accelerometer, temperature, and gyroscope raw values.
- Provides a wake-device operation through `PWR_MGMT_1` (`0x6B`).
- Allows writes to the gyroscope configuration register (`0x1B`).
- Allows writes to the accelerometer configuration register (`0x1C`).

The current implementation displays raw sensor counts; it does not convert them to g, °C, or °/s using the configured full-scale ranges.

### BH1750

- Powers on and triggers a high-resolution measurement.
- Reads the two-byte measurement and reports an approximate lux value.
- Supports power-on, power-down, data-register reset, and selected measurement-mode commands.

Measurement duration and conversion behavior can vary by selected mode and sensor variant. Allow the sensor the required conversion time before reading.

### DS3231

- Reads RTC date/time registers.
- Reads the onboard temperature register.
- Allows setting date/time in 24-hour mode.
- Allows writing the control register (`0x0E`).

The date/time input uses basic range checks; it does not fully validate month lengths or leap years. Verify the resulting RTC time after a write. Writing the RTC changes the device's stored clock.

### TI BQ4050

- Reads Smart Battery System (SBS) word commands at the configured 7-bit address (commonly `0x0B`).
- Reports temperature, voltage, signed current, relative state of charge, remaining and full-charge capacity, battery status, and cycle count.
- Converts SBS temperature (0.1 K) to °C and voltage (mV) to V. Capacity units follow the SBS `BatteryMode` capacity-mode bit (mAh or 10 mWh).
- Reads BQ4050 ManufacturerAccess diagnostics for safety/PF alerts and status, operation/charging/gauging/manufacturing flags, per-cell voltages/currents/powers, and internal/external temperatures.
- Offers confirmed ManufacturerAccess commands to toggle the LED display or simulate its display button for a visual test.

SBS word values and ManufacturerData words are decoded little-endian. ManufacturerAccess writes can have significant effects; this driver deliberately does not expose FET control, protection changes, calibration, reset, security, or data-flash writes. The LED display press is a display test, not a battery measurement or charger-control operation. Verify the battery pack's address, supported commands, and LED hardware before enabling the sample configuration.

## Data streaming and logs

### Device stream logs

If `log_stream=true`, or stream logging is enabled from the device menu, starting a stream creates a CSV file such as:

```text
logs/BMP280-1_20261013_101530.csv
```

The timestamp in the filename is the local system time when the stream log was opened. CSV rows contain:

- Timestamp
- Device name
- Device type
- I²C address
- Reading text returned by the device driver

A new file is created for each stream session, with the device name and session timestamp in the filename. Ensure the configured log directory is writable.

### General audit log

The application creates a separate audit file such as:

```text
logs/audit_20261013_101500.log
```

Audit entries include timestamps for application startup/shutdown, configuration load, device selection, reads and writes initiated through menus, scan start/completion, stream start/stop, and runtime setting changes. The audit log is separate from the device CSV stream files.

Logs use local Windows system time. Protect log files appropriately if they contain sensitive operational information.

At startup, the application automatically removes its timestamped audit and stream log files that meet `log_retention_days`, oldest first. Set that option to `0` to disable automatic cleanup. Choose **Clean old logs** from the main menu to enter a one-time age in days; enter `0` to cancel. The currently open audit log is retained until a later run. Cleanup only targets files using the application's timestamped `audit_*.log` and `*_YYYYMMDD_HHMMSS.csv` naming patterns.

## Adding another device driver

For example, to add a new `MYDEVICE` driver:

1. Create `drivers/MYDEVICE.cpp`.
2. Include `DeviceDriver.h` and implement a class derived from `Driver`.
3. Implement `title()`, `capabilities()`, `readStream(...)`, and `actions()`.
4. Implement `executeAction(...)` for each action exposed by `actions()`. Keep
   prompts, confirmation, and output formatting in the CLI; drivers should only
   receive typed operation inputs and return results.
5. Add a factory declaration such as `std::unique_ptr<Driver> makeMyDeviceDriver();` to `include/DriverRegistry.h`.
6. Implement the factory in `drivers/MYDEVICE.cpp`.
7. Add the type mapping in `src/DriverRegistry.cpp`, e.g. `if (type == "mydevice") return makeMyDeviceDriver();`.
8. Add `drivers\MYDEVICE.cpp` to the compile command.
9. Add a matching `[device FriendlyName]` section to `config/devices.ini`, using `type=mydevice`.
10. Recompile and test the driver against the target device's datasheet and actual hardware.

The driver should use the shared `Bus` abstraction for I²C operations. Do not duplicate the CP2112 DLL-loading implementation in each driver.

## Hardware and I²C notes

- Connect SDA to SDA, SCL to SCL, and ground to ground.
- Confirm that the target's I/O voltage is compatible with the CP2112 adapter and board.
- Ensure appropriate pull-up resistors are present on SDA and SCL.
- Check the target's address straps and datasheet.
- Avoid attaching or detaching devices while a stream or transfer is active.
- Do not configure two devices at the same address on the same bus unless a mux or separate bus isolates them.
- The application accepts 7-bit addresses in configuration and shifts them to the address format expected by the CP2112 API calls in the bus layer.
- The requested clock must be supported by every device on the bus. Start with 100 kHz when diagnosing an unfamiliar setup.

## Troubleshooting

### `g++` is not recognized

Install MinGW-w64 and add the compiler's `bin` directory to `PATH`. Reopen PowerShell and retry `g++ --version`.

### Configuration file cannot be opened

Run the application from the project root or pass the path to `devices.ini` as the first command-line argument.

### DLL cannot be loaded

- Confirm both DLLs are present and accessible.
- Confirm `bridge_dll` points to `SLABHIDtoSMBus.dll`.
- Confirm the DLL architecture matches the executable.
- Confirm `SLABHIDDevice.dll` is available as a runtime dependency.
- Use a compatible pair of Silicon Labs DLLs from the same SDK package.

### No CP2112 adapter detected

- Check the USB cable and port.
- Check Windows Device Manager for the CP2112/HID device.
- Close other applications that may be using the adapter.
- Verify the adapter's USB VID/PID. The current source uses the default Silicon Labs CP2112 VID/PID (`0x10C4:0xEA90`); a device with customized identifiers may require a source change.

### No successful I²C reads

- Check SDA/SCL wiring and common ground.
- Verify voltage levels and pull-ups.
- Verify the configured 7-bit address.
- Try a lower clock, such as 100 kHz.
- Confirm the device is powered and supports the requested register/read operation.
- Remember that a failed read probe does not prove that the address is empty.

### A device appears at an unexpected address

Check its address-select pin or solder jumper. Examples include BMP280 SDO, MPU-6050 AD0, and BH1750 ADDR. Consult the exact device/module datasheet.

### Compilation errors involving Silicon Labs API functions

The declarations in `include/CP2112Bus.h` must match the installed Silicon Labs SDK API exactly. Compare the declarations and exported function names against the SDK's `SLABCP2112.h` and API documentation. Do not assume that similarly named API versions have identical signatures.

### Device stream log is missing

- Confirm `log_stream=true` or enable logging in the device menu.
- Confirm the process can write to `log_dir`.
- Check whether the stream was started and whether the device read returned data or errors.

## Known limitations

- The code is a starter implementation and has not been validated with every SDK version and device revision.
- Driver reads are intentionally simple; sensor calibration and conversion are incomplete for BMP280 and MPU-6050.
- The BQ4050 driver's manufacturer diagnostics and LED tests have not been validated against physical battery packs. Its advanced ManufacturerData reads require the pack to support the documented BQ4050 command responses.
- The DS3231 date-setting routine does not perform complete calendar validation.
- The interactive scanner uses read probes, not a guaranteed ACK-only I²C probe. Some devices may fail reads at an otherwise valid address.
- Runtime refresh/logging changes are not persisted to the configuration file.
- The current driver model is compile-time registration, not a dynamic plugin system.
- Hardware writes can change device state; double-check addresses and values before confirming operations.

## References

- [Silicon Labs CP2112 product and software page](https://www.silabs.com/interface/usb-bridges/classic/device.cp2112?tab=softwareandtools)
- [AN496 — HID USB-to-SMBus API Specification](https://www.silabs.com/documents/public/application-notes/an496-hid-usb-to-smbus-api-specification.pdf)
- [AN495 — CP2112 Interface Specification](https://www.silabs.com/documents/public/application-notes/an495-cp2112-interface-specification.pdf)
- [CP2112 Datasheet](https://www.silabs.com/documents/public/data-sheets/cp2112-datasheet.pdf)
- [BQ4050 product page and technical documentation](https://www.ti.com/product/BQ4050)
- [BQ4050 Technical Reference Manual (SLUUAQ3A)](https://www.ti.com/lit/ug/sluuaq3a/sluuaq3a.pdf)
- [Smart Battery Data Specification](https://sbs-forum.org/specs/sbdat110.pdf)

# Windows MinGW build targets for the CP2112 Device Console.

# Allow callers to select another compiler while defaulting to g++ on PATH.
CXX ?= g++
# Build a 64-bit executable to match the Silicon Labs runtime DLLs.
# Keep the CLI dynamically linked; its matching MinGW runtime DLLs are copied
# into bin\cli so they cannot collide with the Qt runtime in bin\gui.
CXXFLAGS ?= -m64 -std=c++17 -O2 -Wall -Wextra -Iinclude

# Keep the CLI executable and its MinGW runtime files together.
CLI_DIR := bin/cli
TARGET := $(CLI_DIR)/cp2112_console.exe
# Override TOOLCHAIN_BIN when the selected 64-bit MinGW installation is elsewhere.
TOOLCHAIN_BIN ?= C:/mingw64/bin
TOOLCHAIN_BIN_WIN := $(subst /,\,$(TOOLCHAIN_BIN))
CLI_RUNTIME_DLLS := libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll
CLI_OPTIONAL_RUNTIME_DLLS := libmcfgthread-2.dll
# The icon resource is an intermediate object and must stay outside bin/.
RESOURCE := build/app-res.o
# List every application and device-driver translation unit compiled into the CLI.
SOURCES := \
	src/main.cpp \
	src/Application.cpp \
	src/DriverRegistry.cpp \
	drivers/BMP280.cpp \
	drivers/MPU6050.cpp \
	drivers/BH1750.cpp \
	drivers/DS3231.cpp \
	drivers/BQ4050.cpp \
	drivers/24C32BN.cpp \
	drivers/SSD1306.cpp
# Rebuild when a public header changes.
HEADERS := $(wildcard include/*.h)

# These names are callable directly with mingw32-make.
.PHONY: all build check-toolchain run clean

# Default target: compile the application.
all: build

# Build the executable and all of its prerequisites.
build: $(TARGET)

# Verify the compiler, architecture, and resource compiler before building.
check-toolchain:
	where $(CXX)
	$(CXX) --version
	@$(CXX) -dumpmachine | findstr /I mingw >NUL || (echo g++ is not a MinGW toolchain.& exit /B 1)
	@$(CXX) -dumpmachine | findstr /I x86_64 >NUL || (echo g++ is not a 64-bit MinGW toolchain.& exit /B 1)
	where windres
	windres --version

# Create the directory that receives the CLI executable and its runtime DLLs.
$(CLI_DIR):
	@if not exist "bin" mkdir "bin"
	@if not exist "$(CLI_DIR)" mkdir "$(CLI_DIR)"

# Create the directory used for runtime logs during local execution.
logs:
	@if not exist "logs" mkdir "logs"

# Convert the icon resource script and ICO file into a linkable COFF object.
$(RESOURCE): resources/app.rc resources/cp2112.ico | build-dir
	windres $< -O coff -o $@

# Create the directory for intermediate compiler and resource outputs.
build-dir:
	@if not exist "build" mkdir "build"

# Link the C++ sources and embedded icon resource into the 64-bit executable,
# then copy the dynamic MinGW runtime files from the selected toolchain.
$(TARGET): $(SOURCES) $(HEADERS) $(RESOURCE) | $(CLI_DIR) logs
	$(CXX) $(CXXFLAGS) $(SOURCES) $(RESOURCE) -o $@
	@for %%F in ($(CLI_RUNTIME_DLLS)) do @if not exist "$(TOOLCHAIN_BIN_WIN)\%%F" (echo Required CLI runtime is missing: $(TOOLCHAIN_BIN_WIN)\%%F& exit /B 1) else if not exist "$(CLI_DIR)\%%F" copy "$(TOOLCHAIN_BIN_WIN)\%%F" "$(CLI_DIR)\%%F" >NUL
	@for %%F in ($(CLI_OPTIONAL_RUNTIME_DLLS)) do @if exist "$(TOOLCHAIN_BIN_WIN)\%%F" if not exist "$(CLI_DIR)\%%F" copy "$(TOOLCHAIN_BIN_WIN)\%%F" "$(CLI_DIR)\%%F" >NUL

# Build first, then launch the CLI with the project configuration file.
run: build
	$(TARGET)

# Remove locally generated executables and intermediate resource objects.
clean:
	@if exist "bin\cli\*.exe" del /Q "bin\cli\*.exe"
	@if exist "bin\cli\*.o" del /Q "bin\cli\*.o"
	@if exist "build\*.o" del /Q "build\*.o"

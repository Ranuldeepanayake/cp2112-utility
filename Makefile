# Windows MinGW build targets for the CP2112 Device Console.

# Allow callers to select another compiler while defaulting to g++ on PATH.
CXX ?= g++
# Build a 64-bit executable to match the Silicon Labs runtime DLLs.
# Statically link the MinGW C++/thread runtime so the CLI can share bin/ with
# the Qt GUI without mixing incompatible runtime DLL versions.
CXXFLAGS ?= -m64 -static -static-libgcc -static-libstdc++ -std=c++17 -O2 -Wall -Wextra -Iinclude

# Keep the executable in bin/ so it has the same layout as release packages.
TARGET := bin/cp2112_console.exe
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

# Create the directory that receives the final executable and runtime DLLs.
bin:
	@if not exist "bin" mkdir "bin"

# Create the directory used for runtime logs during local execution.
logs:
	@if not exist "logs" mkdir "logs"

# Convert the icon resource script and ICO file into a linkable COFF object.
$(RESOURCE): resources/app.rc resources/cp2112.ico | build-dir
	windres $< -O coff -o $@

# Create the directory for intermediate compiler and resource outputs.
build-dir:
	@if not exist "build" mkdir "build"

# Link the C++ sources and embedded icon resource into the 64-bit executable.
$(TARGET): $(SOURCES) $(HEADERS) $(RESOURCE) | bin logs
	$(CXX) $(CXXFLAGS) $(SOURCES) $(RESOURCE) -o $@

# Build first, then launch the CLI with the project configuration file.
run: build
	$(TARGET) config\devices.ini

# Remove locally generated executables and intermediate resource objects.
clean:
	@if exist "bin\*.exe" del /Q "bin\*.exe"
	@if exist "bin\*.o" del /Q "bin\*.o"
	@if exist "build\*.o" del /Q "build\*.o"

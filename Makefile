# Windows MinGW build targets for the CP2112 Device Console.

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude

TARGET := bin/cp2112_console.exe
SOURCES := \
	src/main.cpp \
	src/Application.cpp \
	src/DriverRegistry.cpp \
	drivers/BMP280.cpp \
	drivers/MPU6050.cpp \
	drivers/BH1750.cpp \
	drivers/DS3231.cpp \
	drivers/BQ4050.cpp
HEADERS := $(wildcard include/*.h)

.PHONY: all build check-toolchain run clean

all: build

build: $(TARGET)

check-toolchain:
	where $(CXX)
	$(CXX) --version
	@$(CXX) -dumpmachine | findstr /I mingw >NUL || (echo g++ is not a MinGW toolchain.& exit /B 1)

bin:
	@if not exist "bin" mkdir "bin"

logs:
	@if not exist "logs" mkdir "logs"

$(TARGET): $(SOURCES) $(HEADERS) | bin logs
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@

run: build
	$(TARGET) config\devices.ini

clean:
	@if exist "bin\*.exe" del /Q "bin\*.exe"

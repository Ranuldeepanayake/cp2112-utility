@echo off
setlocal

rem ── Adjust these four variables to match your Qt installation ─────────────
set QT_VERSION=6.12.0
set QT_MINGW_KIT=mingw_64
set QT_TOOLS_MINGW=mingw1310_64
set QT_INSTALL_ROOT=C:\Qt
rem ──────────────────────────────────────────────────────────────────────────

set QT_PREFIX=%QT_INSTALL_ROOT%\%QT_VERSION%\%QT_MINGW_KIT%
set QT_NINJA=%QT_INSTALL_ROOT%\Tools\Ninja
set QT_CMAKE=%QT_INSTALL_ROOT%\Tools\CMake_64\bin
set QT_COMPILER_BIN=%QT_INSTALL_ROOT%\Tools\%QT_TOOLS_MINGW%\bin
set PATH=%QT_NINJA%;%QT_CMAKE%;%QT_COMPILER_BIN%;%PATH%

if exist cp2112-utility-gui\build\release\CMakeCache.txt (
    del /Q cp2112-utility-gui\build\release\CMakeCache.txt
)

echo Configuring...
cmake -S cp2112-utility-gui ^
      -B cp2112-utility-gui\build\release ^
      -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH="%QT_PREFIX%" ^
      -DCMAKE_CXX_COMPILER="%QT_COMPILER_BIN%\g++.exe"
if %ERRORLEVEL% neq 0 (
    echo CMake configuration failed.
    exit /B %ERRORLEVEL%
)

echo Building...
cmake --build cp2112-utility-gui\build\release --parallel
if %ERRORLEVEL% neq 0 (
    echo CMake build failed.
    exit /B %ERRORLEVEL%
)

echo Done. Output: bin\gui\cp2112-utility-gui.exe

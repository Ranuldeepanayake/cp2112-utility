cmake_minimum_required(VERSION 3.16)

project(cp2112-utility-gui VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
)

# Build the GUI against the same application service and drivers used by the CLI.
set(CORE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
list(APPEND PROJECT_SOURCES
        ${CORE_ROOT}/src/Application.cpp
        ${CORE_ROOT}/src/DriverRegistry.cpp
        ${CORE_ROOT}/drivers/BMP280.cpp
        ${CORE_ROOT}/drivers/MPU6050.cpp
        ${CORE_ROOT}/drivers/BH1750.cpp
        ${CORE_ROOT}/drivers/DS3231.cpp
        ${CORE_ROOT}/drivers/BQ4050.cpp
        ${CORE_ROOT}/drivers/24C32BN.cpp
        ${CORE_ROOT}/include/Application.h
        ${CORE_ROOT}/include/AppTypes.h
        ${CORE_ROOT}/include/CP2112Bus.h
        ${CORE_ROOT}/include/DeviceDriver.h
        ${CORE_ROOT}/include/DriverRegistry.h
        ${CORE_ROOT}/include/SLABCP2112.h
        ${CORE_ROOT}/resources/app.rc
        ${CORE_ROOT}/resources/cp2112.ico
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(cp2112-utility-gui
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET cp2112-utility-gui APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(cp2112-utility-gui SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(cp2112-utility-gui
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(cp2112-utility-gui PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)
target_include_directories(cp2112-utility-gui PRIVATE ${CORE_ROOT}/include)

# Copy only the runtime files required by this Widgets application into the
# repository's shared bin directory. The executable is a local build output,
# while these DLLs are deliberate repository dependencies for packaging.
if(WIN32)
    set(SHARED_BIN_DIR "${CORE_ROOT}/bin")
    get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(QT_PLATFORM_PLUGIN "${Qt${QT_VERSION_MAJOR}_DIR}/../../../plugins/platforms/qwindows.dll")
    option(CP2112_SKIP_RUNTIME_COPY "Leave repository runtime DLLs unchanged for CI packaging" OFF)
    if(CP2112_SKIP_RUNTIME_COPY)
        add_custom_command(TARGET cp2112-utility-gui POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHARED_BIN_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:cp2112-utility-gui>"
                    "${SHARED_BIN_DIR}/cp2112-utility-gui.exe"
            COMMENT "Copying GUI executable to bin")
    else()
        add_custom_command(TARGET cp2112-utility-gui POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHARED_BIN_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHARED_BIN_DIR}/platforms"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Core>"
                    "${SHARED_BIN_DIR}/$<TARGET_FILE_NAME:Qt${QT_VERSION_MAJOR}::Core>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Gui>"
                    "${SHARED_BIN_DIR}/$<TARGET_FILE_NAME:Qt${QT_VERSION_MAJOR}::Gui>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Widgets>"
                    "${SHARED_BIN_DIR}/$<TARGET_FILE_NAME:Qt${QT_VERSION_MAJOR}::Widgets>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${MINGW_BIN_DIR}/libgcc_s_seh-1.dll" "${SHARED_BIN_DIR}/libgcc_s_seh-1.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${MINGW_BIN_DIR}/libstdc++-6.dll" "${SHARED_BIN_DIR}/libstdc++-6.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${MINGW_BIN_DIR}/libwinpthread-1.dll" "${SHARED_BIN_DIR}/libwinpthread-1.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${QT_PLATFORM_PLUGIN}" "${SHARED_BIN_DIR}/platforms/qwindows.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:cp2112-utility-gui>"
                    "${SHARED_BIN_DIR}/cp2112-utility-gui.exe"
            COMMENT "Copying GUI executable and required runtime files to bin")
    endif()
endif()

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.cp2112-utility-gui)
endif()
set_target_properties(cp2112-utility-gui PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS cp2112-utility-gui
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(cp2112-utility-gui)
endif()

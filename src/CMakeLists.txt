cmake_minimum_required(VERSION 3.20)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if(EXISTS "${picoVscode}")
    # Load the extension tool hints even when PICO_SDK_PATH was provided by VS Code.
    # That extension supplies the matching Windows-host pioasm package directory.
    include("${picoVscode}")
endif()
# ====================================================================================

# The SDK needs a WINDOWS-native pioasm tool, not arm-none-eabi-g++.
# If the extension's hint wasn't loaded, discover its standard SDK-matched install.
# Only select a directory containing an actual CMake package configuration.
# Leaving pioasm_DIR unset allows the Pico SDK's normal native-tool fallback.
if(WIN32 AND NOT pioasm_DIR)
    set(_dc_ogx_pioasm_candidate "${USERHOME}/.pico-sdk/tools/${sdkVersion}/pioasm")
    if(EXISTS "${_dc_ogx_pioasm_candidate}/pioasmConfig.cmake" OR
       EXISTS "${_dc_ogx_pioasm_candidate}/pioasm-config.cmake")
        set(pioasm_DIR "${_dc_ogx_pioasm_candidate}" CACHE PATH
            "Matching prebuilt Windows pioasm for Pico SDK ${sdkVersion}")
    endif()
endif()

set(PICO_BOARD pico_w_2023 CACHE STRING "Board type")
set_property(CACHE PICO_BOARD PROPERTY STRINGS pico_w_2023 pico_w pico2_w)
if(NOT PICO_BOARD STREQUAL "pico_w" AND NOT PICO_BOARD STREQUAL "pico_w_2023" AND NOT PICO_BOARD STREQUAL "pico2_w")
    message(FATAL_ERROR "Supported boards are pico_w_2023 (ESP8285), pico_w and pico2_w (official CYW43439)")
endif()
if(PICO_BOARD STREQUAL "pico2_w")
    # Keep one Arm firmware baseline; do not silently select the RISC-V target.
    set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")
    if(NOT PICO_PLATFORM STREQUAL "rp2350-arm-s")
        message(FATAL_ERROR "The Pico 2 W baseline uses PICO_PLATFORM=rp2350-arm-s")
    endif()
endif()
if(DEFINED DC_OGX_CONFIGURED_BOARD AND NOT DC_OGX_CONFIGURED_BOARD STREQUAL PICO_BOARD)
    message(FATAL_ERROR
        "This build directory is configured for ${DC_OGX_CONFIGURED_BOARD}. "
        "Use the ${PICO_BOARD} board's separate CMake preset or a new build directory.")
endif()
set(DC_OGX_CONFIGURED_BOARD "${PICO_BOARD}" CACHE INTERNAL "Board associated with this build directory")
list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards")
list(REMOVE_DUPLICATES PICO_BOARD_HEADER_DIRS)

include(pico_sdk_import.cmake)
project(dc-ogx-w VERSION 0.2.4 LANGUAGES C CXX ASM)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# A Pico W image must fit in a 1000 KiB staging slot, even though the board
# has 2 MiB of flash. GCC/CMake Release normally builds with -O3 (speed),
# which is unnecessarily expensive for the Bluetooth + Wi-Fi firmware.
# Change ONLY Release optimization to -Os, for the app, SDK and dependencies.
# Keep Debug as-is and leave the linker and OTA storage boundaries unchanged.
option(DC_OGX_SIZE_OPTIMIZED_RELEASE
       "Optimize release firmware for flash footprint instead of maximum speed" ON)
if(DC_OGX_SIZE_OPTIMIZED_RELEASE AND CMAKE_BUILD_TYPE STREQUAL "Release"
        AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    foreach(_lang C CXX)
        set(_flag_name "CMAKE_${_lang}_FLAGS_RELEASE")
        set(_original_flags "${${_flag_name}}")
        string(REPLACE "-O3" "-Os" _size_flags "${_original_flags}")
        if(_size_flags STREQUAL _original_flags AND
           NOT _size_flags MATCHES "(^| )-Os( |$)")
            string(APPEND _size_flags " -Os")
        endif()
        set(${_flag_name} "${_size_flags}")
    endforeach()
    message(STATUS "dc-ogx-w: size-optimized Release (-Os), 1000 KiB OTA slot unchanged")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Git REQUIRED)
set(DC_OGX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(DC_OGX_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${DC_OGX_GENERATED}")

include(cmake/options.cmake)
include(cmake/dependencies.cmake)

# Set the external controller stacks before the SDK creates its USB/BT targets.
if(DC_OGX_HAS_BLUETOOTH)
    set(PICO_BTSTACK_PATH "${DC_OGX_BTSTACK_SOURCE_DIR}")
endif()
set(PICO_TINYUSB_PATH "${tinyusb_SOURCE_DIR}")
pico_sdk_init()
if(DC_OGX_USB_HOST_ENABLED)
    # Some SDK/TinyUSB versions register this target before pico_sdk_init;
    # others only expose the function after SDK initialization.
    if(COMMAND check_and_add_pico_pio_usb_support)
        check_and_add_pico_pio_usb_support()
    endif()
    if(NOT TARGET tinyusb_pico_pio_usb)
        message(FATAL_ERROR "TinyUSB PIO USB host target unavailable; check Pico SDK, TinyUSB fork and PICO_PIO_USB_PATH")
    endif()
endif()
if(DC_OGX_HAS_BLUETOOTH)
    include(cmake/sdk_compat.cmake)
endif()

include(cmake/firmware_sources.cmake)
add_executable(dc-ogx-w
    ${DC_OGX_FIRMWARE_SOURCES}
    lib/libxsm3/xsm3.c
    lib/libxsm3/usbdsec.c
    lib/libxsm3/excrypt_des.c
    lib/libxsm3/excrypt_parve.c
    lib/libxsm3/excrypt_sha.c
)

# Keep Curr5's size optimization for every non-USB component. Only the
# selected USB host, TinyUSB/PIO stack and their existing Core0 integration
# use the uploaded reference's Release speed options; source bodies and radio
# dependency options remain unchanged. No compile-LTO option is introduced.
if(DC_OGX_USB_HOST_ENABLED AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(_dc_usb_timing_sources
        src/OGXMini/Board/PicoW.cpp
        src/TaskQueue/TaskQueue.cpp
        src/Board/board_api_private/board_api_usbh.cpp
        src/USBHost/tuh_callbacks.cpp
        src/USBHost/HostDriver/SwitchPro/SwitchPro.cpp
        src/USBHost/HostDriver/PS5/PS5.cpp
        src/USBHost/HostDriver/PS4/PS4.cpp
        src/USBHost/HostDriver/XInput/XboxOne.cpp
        src/USBHost/HostDriver/XInput/tuh_xinput/tuh_xinput.cpp
    )
    file(GLOB_RECURSE _dc_usb_stack_sources CONFIGURE_DEPENDS
        "${PICO_TINYUSB_PATH}/src/*.c" "${PICO_PIO_USB_PATH}/src/*.c")
    list(APPEND _dc_usb_timing_sources ${_dc_usb_stack_sources})
    set_property(SOURCE ${_dc_usb_timing_sources} APPEND PROPERTY COMPILE_OPTIONS
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:Release>:-fomit-frame-pointer>"
        "$<$<CONFIG:Release>:-finline-functions>")
endif()

target_include_directories(dc-ogx-w PRIVATE
    "${DC_OGX_SRC}"
    "${DC_OGX_SRC}/NetworkService"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libxsm3"
    "${DC_OGX_GENERATED}"
)
target_compile_definitions(dc-ogx-w PRIVATE XSM3_NO_DEBUGGING=1 XSM3_EMBEDDED=1)

add_subdirectory("${libfixmath_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/libfixmath-build")

if(DC_OGX_HAS_BLUETOOTH)
    add_subdirectory("${DC_OGX_BLUEPAD32_SOURCE_DIR}/src/components/bluepad32"
                     "${CMAKE_CURRENT_BINARY_DIR}/_deps/bluepad32-build")

    # Bluetooth uses the 2026 source plus exactly one verified BTstack HIDS fix.

    # Bluetooth and Wi-Fi share one CYW43 state and background async context.
    # Bluepad32's arch_none must not propagate CYW43_LWIP=0 to consumers.
    foreach(_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_links bluepad32 ${_property})
        string(REPLACE "pico_cyw43_arch_none" "pico_cyw43_arch_lwip_threadsafe_background" _links "${_links}")
        set_property(TARGET bluepad32 PROPERTY ${_property} "${_links}")
    endforeach()
    target_include_directories(bluepad32 PRIVATE "${DC_OGX_SRC}" "${DC_OGX_SRC}/NetworkService")
    target_compile_definitions(bluepad32 PRIVATE OGXM_BLUEPAD32_PICO_W=1)
    target_link_libraries(dc-ogx-w PRIVATE
        pico_btstack_classic pico_btstack_cyw43 bluepad32
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip_mdns
    )
endif()
target_compile_definitions(libfixmath PRIVATE
    FIXMATH_FAST_SIN FIXMATH_NO_64BIT FIXMATH_NO_CACHE
    FIXMATH_NO_HARD_DIVISION FIXMATH_NO_OVERFLOW
)

target_link_libraries(dc-ogx-w PRIVATE
    pico_stdlib pico_multicore pico_rand pico_unique_id pico_flash
    hardware_timer hardware_clocks hardware_flash hardware_uart hardware_i2c hardware_irq
    hardware_pio hardware_dma hardware_watchdog
    tinyusb_device tinyusb_board
    libfixmath
)
if(DC_OGX_USB_HOST_ENABLED)
    target_link_libraries(dc-ogx-w PRIVATE tinyusb_host tinyusb_pico_pio_usb)
endif()

include(cmake/generated.cmake)
pico_generate_pio_header(dc-ogx-w "${DC_OGX_SRC}/Board/Pico_WS2812/WS2812.pio")

# USB remains dedicated to controller output. Debug logging uses UART1 GP4/GP5.
pico_enable_stdio_usb(dc-ogx-w 0)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    pico_enable_stdio_uart(dc-ogx-w 1)
    target_compile_definitions(dc-ogx-w PRIVATE
        PICO_DEFAULT_UART=1 PICO_DEFAULT_UART_TX_PIN=4 PICO_DEFAULT_UART_RX_PIN=5
    )
else()
    pico_enable_stdio_uart(dc-ogx-w 0)
endif()

pico_set_program_name(dc-ogx-w "dc-ogx-w")
pico_set_program_version(dc-ogx-w "${PROJECT_VERSION}")
pico_add_extra_outputs(dc-ogx-w)
dc_ogx_add_update_package()
message(STATUS "dc-ogx-w board: ${PICO_BOARD}; Bluetooth: ${DC_OGX_HAS_BLUETOOTH}")

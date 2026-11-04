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
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

set(PICO_BOARD pico_w_2023 CACHE STRING "Board type")
set_property(CACHE PICO_BOARD PROPERTY STRINGS pico_w_2023 pico_w)
if(NOT PICO_BOARD STREQUAL "pico_w" AND NOT PICO_BOARD STREQUAL "pico_w_2023")
    message(FATAL_ERROR "Supported boards are pico_w_2023 (ESP8285) and pico_w (official CYW43439)")
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

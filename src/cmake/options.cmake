set(OGXM_WIFI_SSID_PREFIX "dc-ogx-w" CACHE STRING "SoftAP name prefix; board suffix is appended")
set(OGXM_WIFI_PASSWORD "ogxremote" CACHE STRING "SoftAP WPA2 passphrase")
set(OGXM_WIFI_CHANNEL 6 CACHE STRING "SoftAP channel, 1-11")
set(OGXM_FIXED_DRIVER "" CACHE STRING "Optional fixed output mode, e.g. XINPUT or XBOXOG; empty allows mode combos")
option(OGXM_FIXED_DRIVER_ALLOW_COMBOS "Allow mode combos when a default output mode is set" OFF)
set(MAIN_LOOP_DELAY_US 0 CACHE STRING "Main loop delay in microseconds")
option(OGXM_STATUS_LED_ENABLED "Enable one external WS2812 status LED" ON)
set(OGXM_STATUS_LED_PIN 8 CACHE STRING "External WS2812 data GPIO")
set(OGXM_STATUS_LED_BRIGHTNESS 24 CACHE STRING "WS2812 brightness, 0-255")
if(NOT OGXM_STATUS_LED_PIN MATCHES "^[0-9]+$" OR OGXM_STATUS_LED_PIN GREATER 29)
    message(FATAL_ERROR "OGXM_STATUS_LED_PIN must be a GPIO number from 0 to 29")
endif()
if(NOT OGXM_STATUS_LED_BRIGHTNESS MATCHES "^[0-9]+$" OR OGXM_STATUS_LED_BRIGHTNESS GREATER 255)
    message(FATAL_ERROR "OGXM_STATUS_LED_BRIGHTNESS must be between 0 and 255")
endif()
if(OGXM_STATUS_LED_ENABLED)
    add_compile_definitions(OGXM_STATUS_LED_ENABLED=1)
else()
    add_compile_definitions(OGXM_STATUS_LED_ENABLED=0)
endif()
add_compile_definitions(OGXM_STATUS_LED_PIN=${OGXM_STATUS_LED_PIN}
                        OGXM_STATUS_LED_BRIGHTNESS=${OGXM_STATUS_LED_BRIGHTNESS})

# Optional SSD1306 status panel (I2C1 GP14/GP15). No OLED is required to boot.
option(DC_OGX_OLED_ENABLED "Enable optional I2C SSD1306 status panel" ON)
set(DC_OGX_OLED_SDA 14 CACHE STRING "OLED SDA pin (RP2040 I2C1)")
set(DC_OGX_OLED_SCL 15 CACHE STRING "OLED SCL pin (RP2040 I2C1)")
set(DC_OGX_OLED_ADDRESS 0x3c CACHE STRING "SSD1306 I2C address (0x3c or 0x3d)")
set(DC_OGX_OLED_HEIGHT 32 CACHE STRING "SSD1306 height (supported UI: 32)")
set(DC_OGX_OLED_CONTRAST 224 CACHE STRING "OLED active contrast, 0-255")
option(DC_OGX_OLED_FADE_ENABLED "Fade the SSD1306 contrast on startup" ON)
set(DC_OGX_OLED_IDLE_DIM_MS 0 CACHE STRING "Optional idle dim delay in ms; 0 disables dimming")
set(DC_OGX_OLED_IDLE_CONTRAST 32 CACHE STRING "OLED idle dim contrast, 0-255")
foreach(_val DC_OGX_OLED_CONTRAST DC_OGX_OLED_IDLE_CONTRAST)
    if(NOT "${${_val}}" MATCHES "^[0-9]+$" OR ${_val} GREATER 255)
        message(FATAL_ERROR "${_val} must be 0..255")
    endif()
endforeach()
if(NOT DC_OGX_OLED_IDLE_DIM_MS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "DC_OGX_OLED_IDLE_DIM_MS must be >= 0")
endif()
if(NOT DC_OGX_OLED_SDA STREQUAL "14" OR NOT DC_OGX_OLED_SCL STREQUAL "15")
    message(FATAL_ERROR "This build supports SSD1306 on I2C1 SDA GP14 / SCL GP15 only")
endif()
if(NOT DC_OGX_OLED_ADDRESS STREQUAL "0x3c" AND NOT DC_OGX_OLED_ADDRESS STREQUAL "0x3d")
    message(FATAL_ERROR "DC_OGX_OLED_ADDRESS must be 0x3c or 0x3d")
endif()
if(NOT DC_OGX_OLED_HEIGHT STREQUAL "32")
    message(FATAL_ERROR "This OLED renderer targets the tested 128x32 panel; 128x64 is not supported yet")
endif()
if(DC_OGX_OLED_ENABLED AND OGXM_STATUS_LED_ENABLED AND OGXM_STATUS_LED_PIN MATCHES "^(14|15)$")
    message(FATAL_ERROR "OLED I2C pins conflict with WS2812 pin")
endif()
if(DC_OGX_OLED_ENABLED)
    set(_oled_flag 1)
else()
    set(_oled_flag 0)
endif()
add_compile_definitions(DC_OGX_OLED_ENABLED=${_oled_flag}
                        DC_OGX_OLED_SDA=${DC_OGX_OLED_SDA}
                        DC_OGX_OLED_SCL=${DC_OGX_OLED_SCL}
                        DC_OGX_OLED_ADDRESS=${DC_OGX_OLED_ADDRESS}
                        DC_OGX_OLED_HEIGHT=${DC_OGX_OLED_HEIGHT}
                        DC_OGX_OLED_CONTRAST=${DC_OGX_OLED_CONTRAST}
                        DC_OGX_OLED_IDLE_DIM_MS=${DC_OGX_OLED_IDLE_DIM_MS}
                        DC_OGX_OLED_IDLE_CONTRAST=${DC_OGX_OLED_IDLE_CONTRAST})
if(DC_OGX_OLED_FADE_ENABLED)
    add_compile_definitions(DC_OGX_OLED_FADE_ENABLED=1)
else()
    add_compile_definitions(DC_OGX_OLED_FADE_ENABLED=0)
endif()

# Shared reference USB host/device path on the DC-OGX-W carrier PCB.
# Native USB stays as a console USB device; PIO-USB uses consecutive GP9/GP10.
set(DC_OGX_USB_HOST_DP_PIN 9 CACHE STRING "USB host D+ RP2040 GPIO")
set(DC_OGX_USB_HOST_DM_PIN 10 CACHE STRING "USB host D- RP2040 GPIO")
option(DC_OGX_USB_HOST_ENABLED "PIO USB wired controller input" ON)
if(NOT DC_OGX_USB_HOST_DP_PIN STREQUAL "9" OR NOT DC_OGX_USB_HOST_DM_PIN STREQUAL "10")
    message(FATAL_ERROR "DC-OGX-W USB host PCB is wired GP9 (D+) / GP10 (D-)")
endif()
if(DC_OGX_USB_HOST_ENABLED)
    if(OGXM_STATUS_LED_ENABLED AND (OGXM_STATUS_LED_PIN EQUAL 9 OR OGXM_STATUS_LED_PIN EQUAL 10))
        message(FATAL_ERROR "PIO-USB GP9/GP10 conflicts with WS2812 GPIO")
    endif()
    add_compile_definitions(CONFIG_EN_USB_HOST=1)
endif()

# Board capabilities are fixed by the hardware, not independent feature toggles.
# Pico 2 W has its own overclock target; keep all RP2040 boards at their existing
# 240 MHz default. The CMake cache/presets can still override this value.
if(PICO_BOARD STREQUAL "pico2_w")
    set(_dc_ogx_default_sysclock_khz 270000)
else()
    set(_dc_ogx_default_sysclock_khz 240000)
endif()
set(SYSCLOCK_KHZ "${_dc_ogx_default_sysclock_khz}" CACHE STRING "System clock in kHz (shared PIO USB timing)")
unset(_dc_ogx_default_sysclock_khz)
if(PICO_BOARD STREQUAL "pico2_w")
    set(DC_OGX_HAS_BLUETOOTH TRUE)
    add_compile_definitions(CONFIG_OGXM_BOARD_PI_PICO2W=1 CONFIG_EN_BLUETOOTH=1)
elseif(PICO_BOARD STREQUAL "pico_w")
    set(DC_OGX_HAS_BLUETOOTH TRUE)
    add_compile_definitions(CONFIG_OGXM_BOARD_PI_PICOW=1 CONFIG_EN_BLUETOOTH=1)
elseif(PICO_BOARD STREQUAL "pico_w_2023")
    set(DC_OGX_HAS_BLUETOOTH FALSE)
    if(OGXM_WIFI_PASSWORD MATCHES ",")
        message(FATAL_ERROR "The ESP8285 AT transport does not support commas in OGXM_WIFI_PASSWORD")
    endif()
    add_compile_definitions(CONFIG_OGXM_BOARD_PI_PICOW_2023=1)
    set(OGXM_ESP_UART_ID 0 CACHE STRING "ESP8285 UART controller")
    set(OGXM_ESP_UART_TX_PIN 0 CACHE STRING "RP2040 TX pin connected to ESP8285 RX")
    set(OGXM_ESP_UART_RX_PIN 1 CACHE STRING "RP2040 RX pin connected to ESP8285 TX")
    set(OGXM_ESP_UART_BAUD 115200 CACHE STRING "ESP8285 AT firmware UART baud rate")
    if(NOT OGXM_ESP_UART_ID MATCHES "^[01]$")
        message(FATAL_ERROR "OGXM_ESP_UART_ID must be 0 or 1")
    endif()
    foreach(_pin OGXM_ESP_UART_TX_PIN OGXM_ESP_UART_RX_PIN)
        if(NOT "${${_pin}}" MATCHES "^[0-9]+$" OR ${_pin} GREATER 29)
            message(FATAL_ERROR "${_pin} must be a GPIO number from 0 to 29")
        endif()
    endforeach()
    if(OGXM_ESP_UART_TX_PIN EQUAL OGXM_ESP_UART_RX_PIN)
        message(FATAL_ERROR "ESP8285 UART TX and RX must use different GPIOs")
    endif()
    if(DC_OGX_USB_HOST_ENABLED AND
       (OGXM_ESP_UART_TX_PIN EQUAL 9 OR OGXM_ESP_UART_TX_PIN EQUAL 10 OR
        OGXM_ESP_UART_RX_PIN EQUAL 9 OR OGXM_ESP_UART_RX_PIN EQUAL 10))
        message(FATAL_ERROR "ESP8285 UART cannot share PIO-USB GP9/GP10")
    endif()
    if(OGXM_STATUS_LED_ENABLED AND
       (OGXM_STATUS_LED_PIN EQUAL OGXM_ESP_UART_TX_PIN OR OGXM_STATUS_LED_PIN EQUAL OGXM_ESP_UART_RX_PIN))
        message(FATAL_ERROR "WS2812 data GPIO conflicts with the ESP8285 UART")
    endif()
    if(OGXM_ESP_UART_ID EQUAL 0)
        set(_uart_tx_pins 0 12 16 28)
        set(_uart_rx_pins 1 13 17 29)
    else()
        set(_uart_tx_pins 4 8 20 24)
        set(_uart_rx_pins 5 9 21 25)
    endif()
    if(NOT OGXM_ESP_UART_TX_PIN IN_LIST _uart_tx_pins OR NOT OGXM_ESP_UART_RX_PIN IN_LIST _uart_rx_pins)
        message(FATAL_ERROR "ESP8285 UART pins do not support RP2040 UART${OGXM_ESP_UART_ID} TX/RX")
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND OGXM_ESP_UART_ID EQUAL 1)
        message(FATAL_ERROR "ESP8285 UART1 conflicts with the debug UART1 on GP4/GP5")
    endif()
    if(NOT OGXM_ESP_UART_BAUD MATCHES "^[0-9]+$" OR OGXM_ESP_UART_BAUD LESS 9600 OR OGXM_ESP_UART_BAUD GREATER 2000000)
        message(FATAL_ERROR "OGXM_ESP_UART_BAUD must be between 9600 and 2000000")
    endif()
    add_compile_definitions(
        OGXM_ESP_UART_ID=${OGXM_ESP_UART_ID}
        OGXM_ESP_UART_TX_PIN=${OGXM_ESP_UART_TX_PIN}
        OGXM_ESP_UART_RX_PIN=${OGXM_ESP_UART_RX_PIN}
        OGXM_ESP_UART_BAUD=${OGXM_ESP_UART_BAUD}
    )
endif()

if(NOT SYSCLOCK_KHZ MATCHES "^[0-9]+$")
    message(FATAL_ERROR "SYSCLOCK_KHZ must be an integer clock frequency in kHz")
endif()

string(LENGTH "${OGXM_WIFI_SSID_PREFIX}" _ssid_length)
string(LENGTH "${OGXM_WIFI_PASSWORD}" _password_length)
if(_ssid_length LESS 1 OR _ssid_length GREATER 27 OR NOT OGXM_WIFI_SSID_PREFIX MATCHES "^[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "SoftAP prefix must be 1-27 letters, digits, underscores or hyphens")
endif()
if(_password_length LESS 8 OR _password_length GREATER 63 OR NOT OGXM_WIFI_PASSWORD MATCHES "^[A-Za-z0-9_!@%+=.,:-]+$")
    message(FATAL_ERROR "SoftAP passphrase must be 8-63 letters, digits or _!@%+=.,:-")
endif()
if(NOT OGXM_WIFI_CHANNEL MATCHES "^[0-9]+$" OR OGXM_WIFI_CHANNEL LESS 1 OR OGXM_WIFI_CHANNEL GREATER 11)
    message(FATAL_ERROR "OGXM_WIFI_CHANNEL must be 1-11")
endif()

# Keep configure/build outputs independent of wall-clock time. The legacy
# UserSettings API still calls this BUILD_DATETIME, but it only needs a stable
# firmware identity; changing it on every configure rebuilds the entire image.
add_compile_definitions(
    MAX_GAMEPADS=1 CONFIG_OGXM_USB_DEVICE_ONLY=1 CONFIG_OGXM_NETWORK=1
    FIRMWARE_NAME="dc-ogx-w" FIRMWARE_VERSION="${PROJECT_VERSION}"
    OGXM_REMOTE_VERSION="${PROJECT_VERSION}" BUILD_DATETIME="dc-ogx-w-${PROJECT_VERSION}"
    PICO_NO_BI_PROGRAM_BUILD_DATE=1
    OGXM_WIFI_SSID_PREFIX="${OGXM_WIFI_SSID_PREFIX}"
    OGXM_WIFI_PASSWORD="${OGXM_WIFI_PASSWORD}" OGXM_WIFI_CHANNEL=${OGXM_WIFI_CHANNEL}
    MAIN_LOOP_DELAY_US=${MAIN_LOOP_DELAY_US}
    SYSCLOCK_KHZ=${SYSCLOCK_KHZ}
    PICO_FLASH_SIZE_BYTES=2097152 NVS_SECTORS=4
    PICO_STACK_SIZE=0x2000 PICO_CORE1_STACK_SIZE=0x2000
    "PICO_FLASH_BANK_STORAGE_OFFSET=(PICO_FLASH_SIZE_BYTES-24576)"
    PICO_FLASH_BANK_TOTAL_SIZE=8192
)

if(OGXM_FIXED_DRIVER)
    set(_non_usb_outputs WII PS1PS2 GAMECUBE DREAMCAST N64)
    if(OGXM_FIXED_DRIVER IN_LIST _non_usb_outputs)
        message(FATAL_ERROR "Both targets use the built-in USB connector for output; ${OGXM_FIXED_DRIVER} is not a USB output mode")
    endif()
    set(_drivers XBOXOG XBOXOG_SB XBOXOG_XR XINPUT PS3 DINPUT PSCLASSIC SWITCH WIIU WII PS1PS2 GAMECUBE DREAMCAST N64 PS4 STEAM)
    list(FIND _drivers "${OGXM_FIXED_DRIVER}" _driver_index)
    if(OGXM_FIXED_DRIVER STREQUAL "WEBAPP")
        set(_driver_id 100)
    elseif(_driver_index GREATER_EQUAL 0)
        math(EXPR _driver_id "${_driver_index}+1")
    else()
        message(FATAL_ERROR "Unknown OGXM_FIXED_DRIVER: ${OGXM_FIXED_DRIVER}")
    endif()
    add_compile_definitions(CONFIG_OGXM_FIXED_DRIVER=1 OGXM_FIXED_DRIVER_TYPE=${_driver_id})
    if(OGXM_FIXED_DRIVER_ALLOW_COMBOS AND NOT OGXM_FIXED_DRIVER STREQUAL "WII")
        add_compile_definitions(CONFIG_OGXM_FIXED_DRIVER_ALLOW_COMBOS=1)
    endif()
endif()

if(EXISTS "${DC_OGX_SRC}/USBDevice/DeviceDriver/XboxOG/tud_xid/tud_xid_xremote_rom.h")
    add_compile_definitions(XREMOTE_ROM_AVAILABLE)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(CFG_TUSB_DEBUG=1 OGXM_DEBUG=1 CONFIG_OGXM_DEBUG=1)
else()
    add_compile_definitions(CFG_TUSB_DEBUG=0)
endif()

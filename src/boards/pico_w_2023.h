/*
 * RP2040-Pico W-2023: RP2040 with an onboard ESP8285 UART Wi-Fi module.
 * This is a distinct Pico SDK board; it does not have a CYW43439 radio.
 * This header is also read by CMake and the assembler.
 */
#ifndef _BOARDS_PICO_W_2023_H
#define _BOARDS_PICO_W_2023_H

pico_board_cmake_set(PICO_PLATFORM, rp2040)
pico_board_cmake_set(PICO_CYW43_SUPPORTED, 0)

#define RP2040_PICO_W_2023 1

// Debug UART is separate from the ESP8285 UART0 connection on GP0/GP1.
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 1
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 4
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 5
#endif

// Keep the existing firmware/storage layout within the first 2 MiB of flash.
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

#ifndef PICO_RP2040_B0_SUPPORTED
#define PICO_RP2040_B0_SUPPORTED 1
#endif

// Do not inherit pico.h's LED, SMPS, VBUS or VSYS GPIO assumptions.
// ESP8285 reset/enable/boot wiring has not been verified for this board.

#endif

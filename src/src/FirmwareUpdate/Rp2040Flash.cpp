#include "FirmwareUpdate/UpdatePlatform.h"
#include "Board/Config.h"
#include "Board/board_api.h"

#include <cstring>
#include <hardware/flash.h>
#include <hardware/regs/psm.h>
#include <hardware/regs/watchdog.h>
#include <hardware/structs/dma.h>
#include <hardware/structs/psm.h>
#include <hardware/structs/watchdog.h>
#include <hardware/sync.h>
#include <pico/bootrom.h>
#include <pico/flash.h>
#include <pico/rand.h>
#include <pico/stdlib.h>

namespace FirmwareUpdate::Platform {
namespace {

#if defined(CONFIG_OGXM_BOARD_PI_PICOW_2023)
constexpr Board CURRENT_BOARD = Board::PicoW2023;
constexpr const char* BOARD_NAME = "pico_w_2023";
#else
constexpr Board CURRENT_BOARD = Board::PicoW;
constexpr const char* BOARD_NAME = "pico_w";
#endif

struct Identity {
    std::uint8_t magic[16];
    std::uint32_t board, layout, max_image, staging;
};
static_assert(sizeof(Identity) == 32);
// Referenced by a volatile read below, so --gc-sections retains this marker.
__attribute__((used, section(".rodata.dc_ogx_update_identity")))
const Identity image_identity = {
    {'D','C','O','G','X','W','-','I','M','A','G','E','-','V','1',0},
    static_cast<std::uint32_t>(CURRENT_BOARD), LAYOUT_VERSION, MAX_IMAGE_SIZE, STAGING_OFFSET
};

Board current_board() {
    const volatile Identity* identity = &image_identity;
    return static_cast<Board>(identity->board);
}

struct FlashWrite {
    std::uint32_t offset;
    const std::uint8_t* bytes;
    bool erase;
};

void __no_inline_not_in_flash_func(stage_page)(void* opaque) {
    auto* operation = static_cast<FlashWrite*>(opaque);
    if (operation->erase) flash_range_erase(operation->offset, SECTOR_SIZE);
    flash_range_program(operation->offset, operation->bytes, PAGE_SIZE);
}

bool write_stage(std::uint32_t offset, const std::uint8_t* bytes, std::size_t length) {
    if (offset > MAX_IMAGE_SIZE || length > MAX_IMAGE_SIZE - offset || offset % PAGE_SIZE) return false;
    alignas(4) std::uint8_t page[PAGE_SIZE];
    for (std::size_t done = 0; done < length; done += PAGE_SIZE) {
        const std::size_t available = length - done < PAGE_SIZE ? length - done : PAGE_SIZE;
        std::memset(page, 0xff, sizeof page);
        std::memcpy(page, bytes + done, available);
        const std::uint32_t address = STAGING_OFFSET + offset + static_cast<std::uint32_t>(done);
        FlashWrite operation{address, page, (address % SECTOR_SIZE) == 0};
        if (flash_safe_execute(stage_page, &operation, 100) != PICO_OK) return false;
    }
    return true;
}

// Everything after the first app-sector erase is SRAM or ROM code. Resolve
// ROM entry points before entering this routine, use one SRAM sector buffer,
// and avoid libc, SDK sleep/watchdog wrappers, logging, or C++ destructors.
struct InstallContext {
    rom_connect_internal_flash_fn connect;
    rom_flash_exit_xip_fn exit_xip;
    rom_flash_range_erase_fn erase;
    rom_flash_range_program_fn program;
    rom_flash_flush_cache_fn flush;
    rom_flash_enter_cmd_xip_fn enter_xip;
    rom_reset_usb_boot_fn usb_boot;
    std::uint32_t image_size;
    alignas(4) std::uint8_t sector[SECTOR_SIZE];
};
InstallContext install_context;

[[noreturn]] void __no_inline_not_in_flash_func(install_from_ram)(InstallContext* context) {
    // Watchdog must not interrupt the copy. It is configured for a clean boot
    // only once all destination sectors have been read back successfully.
    watchdog_hw->ctrl = 0;
    dma_hw->abort = DMA_CHAN_ABORT_BITS;
    while (dma_hw->abort) { __asm volatile("nop"); }
    const std::uint32_t sectors = (context->image_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    for (std::uint32_t sector = 0; sector < sectors; ++sector) {
        const std::uint32_t offset = sector * SECTOR_SIZE;
        const volatile std::uint8_t* source =
            reinterpret_cast<const volatile std::uint8_t*>(FLASH_BASE + STAGING_OFFSET + offset);
        for (std::uint32_t i = 0; i < SECTOR_SIZE; ++i)
            context->sector[i] = offset + i < context->image_size ? source[i] : 0xff;
        context->connect();
        context->exit_xip();
        context->erase(offset, SECTOR_SIZE, SECTOR_SIZE, 0x20);
        context->program(offset, context->sector, SECTOR_SIZE);
        context->flush();
        // Generic 03h command XIP is ROM code and independent of either
        // image's boot2, including after we have replaced sector zero.
        context->enter_xip();
        const volatile std::uint8_t* target = reinterpret_cast<const volatile std::uint8_t*>(FLASH_BASE + offset);
        for (std::uint32_t i = 0; i < SECTOR_SIZE; ++i) {
            if (target[i] != context->sector[i]) {
                context->usb_boot(0, 0); // Recovery from ROM, never damaged app.
                while (true) { __asm volatile("nop"); }
            }
        }
    }
    watchdog_hw->scratch[4] = 0; // No stale RAM-reboot address.
    psm_hw->wdsel = PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS);
    watchdog_hw->ctrl = WATCHDOG_CTRL_TRIGGER_BITS;
    while (true) { __asm volatile("nop"); }
}


} // namespace

Board board() { return current_board(); }
const char* board_name() { return BOARD_NAME; }
std::uint32_t random_token() { return get_rand_32(); }
Storage storage() { return {write_stage, reinterpret_cast<const std::uint8_t*>(FLASH_BASE + STAGING_OFFSET)}; }

const char* install(const ImageHeader& header) {
    const char* invalid = validate_image(
        reinterpret_cast<const std::uint8_t*>(FLASH_BASE + STAGING_OFFSET), header);
    if (invalid) return invalid;
    install_context.connect = reinterpret_cast<rom_connect_internal_flash_fn>(rom_func_lookup(ROM_FUNC_CONNECT_INTERNAL_FLASH));
    install_context.exit_xip = reinterpret_cast<rom_flash_exit_xip_fn>(rom_func_lookup(ROM_FUNC_FLASH_EXIT_XIP));
    install_context.erase = reinterpret_cast<rom_flash_range_erase_fn>(rom_func_lookup(ROM_FUNC_FLASH_RANGE_ERASE));
    install_context.program = reinterpret_cast<rom_flash_range_program_fn>(rom_func_lookup(ROM_FUNC_FLASH_RANGE_PROGRAM));
    install_context.flush = reinterpret_cast<rom_flash_flush_cache_fn>(rom_func_lookup(ROM_FUNC_FLASH_FLUSH_CACHE));
    install_context.enter_xip = reinterpret_cast<rom_flash_enter_cmd_xip_fn>(rom_func_lookup(ROM_FUNC_FLASH_ENTER_CMD_XIP));
    install_context.usb_boot = reinterpret_cast<rom_reset_usb_boot_fn>(rom_func_lookup(ROM_FUNC_RESET_USB_BOOT));
    install_context.image_size = header.image_size;
    if (!install_context.connect || !install_context.exit_xip || !install_context.erase ||
        !install_context.program || !install_context.flush || !install_context.enter_xip || !install_context.usb_boot) {
        return "bootrom_api_unavailable";
    }
    board_api::usb::disconnect_all(); // Stops Core1, timers and native USB first.
    save_and_disable_interrupts();
    install_from_ram(&install_context);
}

} // namespace FirmwareUpdate::Platform

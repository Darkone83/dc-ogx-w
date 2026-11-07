// DC-OGX-W | 0.91" 128x32 SSD1306 animated micro UI.
// Drop-in replacement for src/Display/OledDisplay.cpp (v1.1 source package).
// Large-text font masks are rasterized from DejaVu Sans Condensed Bold
// (DejaVu Fonts / Bitstream Vera permissive license). Microfont is an original
// 5x7 bitmap. No graphics library or heap allocation; I2C advances without waiting.
#include "Display/OledDisplay.h"
#include "Display/OledGlyphs.h"
#include "Display/OledSettings.h"
#include "NetworkService/NetworkService.h"
#include "FirmwareUpdate/FirmwareUpdate.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#ifndef DC_OGX_OLED_ENABLED
#define DC_OGX_OLED_ENABLED 1
#endif
#ifndef DC_OGX_OLED_SDA
#define DC_OGX_OLED_SDA 14
#endif
#ifndef DC_OGX_OLED_SCL
#define DC_OGX_OLED_SCL 15
#endif
#ifndef DC_OGX_OLED_ADDRESS
#define DC_OGX_OLED_ADDRESS 0x3c
#endif
#ifndef DC_OGX_OLED_CONTRAST
#define DC_OGX_OLED_CONTRAST 224
#endif
#ifndef DC_OGX_OLED_FADE_ENABLED
#define DC_OGX_OLED_FADE_ENABLED 1
#endif
#ifndef DC_OGX_OLED_IDLE_DIM_MS
#define DC_OGX_OLED_IDLE_DIM_MS 0
#endif
#ifndef DC_OGX_OLED_IDLE_CONTRAST
#define DC_OGX_OLED_IDLE_CONTRAST 32
#endif

namespace OledDisplay {
namespace {
std::atomic<std::uint32_t> shared{1u << 8};
// Bits: BT connected(0), USB host connected(1), BLE(2), BT kind(8..15), USB kind(16..23).
std::atomic<std::uint32_t> input_sources{0};
std::atomic<bool> online{false};
std::atomic<std::uint32_t> activity_posted{0};
std::atomic<std::uint32_t> activity_seq{0};
#if DC_OGX_OLED_ENABLED
constexpr unsigned kWidth = 128;
constexpr unsigned kHeight = 32;       // Actual panel, not a scaled 128x64 scene.
constexpr unsigned kPages = 4;
// Queue fixed packets through the I2C FIFO; the service never waits for the bus.
// Fill the existing 32-byte packet (one control byte + 31 display bytes).
// advance_transfer still queues at most 16 FIFO writes per visit.
constexpr unsigned kBurst = 31;
static_assert(kBurst+1<=32, "OLED packet exceeds its fixed transfer buffer");
constexpr std::uint32_t kProbeMs = 10000;
constexpr std::uint32_t kSplashMs = 1550;
constexpr std::uint32_t kIpMs = 12000;
constexpr std::uint32_t kProfileMs = 4000;
constexpr std::uint32_t kNetworkMs = 3300;
constexpr std::uint32_t kInputMs = 4400;
constexpr unsigned kWipeSteps = 6;
constexpr std::uint32_t kWipeStepMs = 45;
static_assert(DC_OGX_OLED_ADDRESS == 0x3c || DC_OGX_OLED_ADDRESS == 0x3d, "Invalid SSD1306 address");
static_assert(DC_OGX_OLED_CONTRAST >= 0 && DC_OGX_OLED_CONTRAST <= 255, "Invalid OLED contrast");

bool bus_initialized = false;
bool stream_setup = false;
bool frame_dirty = true;
bool transfer_pending = false;
bool panel_on = true;
std::uint32_t next_probe = 0;
std::uint32_t next_chunk = 0;
std::uint32_t previous_state = ~0u;
std::uint32_t previous_inputs = ~0u;
std::uint8_t previous_network_flags = 0xff;
std::uint32_t page_deadline = 0;
std::uint32_t splash_since = 0;
std::uint8_t page = 0;
std::uint8_t fb[kWidth * kPages]{};
std::uint8_t wipe_source[kWidth * kPages]{};
std::size_t sent = 0;
char previous_ip[16]{};
std::uint32_t observed_activity = 0;
std::uint32_t active_activity_serial = 0;
std::uint32_t activity_deadline = 0;
Activity activity = Activity::None;
FirmwareUpdate::DisplayPhase update_phase = FirmwareUpdate::DisplayPhase::Idle;
std::uint32_t update_phase_since = 0;
std::uint8_t displayed_overlay = 0;
std::uint32_t displayed_event = 0;
FirmwareUpdate::DisplayPhase displayed_phase = FirmwareUpdate::DisplayPhase::Idle;
std::uint8_t displayed_percent = 255;
std::uint8_t animation_frame = 0;
std::uint8_t visible_scene = 0;
std::uint8_t wipe_step = 0;
std::uint8_t wipe_style = 0;
bool wiping = false;
std::uint32_t next_wipe_step = 0;
std::uint32_t next_motion = 0;
std::uint8_t active_contrast = static_cast<std::uint8_t>(DC_OGX_OLED_FADE_ENABLED ? 8 : DC_OGX_OLED_CONTRAST);
std::uint32_t next_contrast_step = 0;
bool contrast_resend_pending = false;
bool brightness_apply_pending = false;
std::uint32_t last_external_activity = 0;
std::uint32_t previous_settings = ~0u;

bool due(std::uint32_t now, std::uint32_t at) {
    return static_cast<std::int32_t>(now - at) >= 0;
}
enum class TxKind : std::uint8_t { None, Initialize, Settings, Contrast, Power, Address, Data };
enum class TxResult : std::uint8_t { Pending, Done, Failed };
struct Transfer {
    TxKind kind{TxKind::None};
    std::uint8_t packet[32]{};
    std::uint8_t size{}, queued{}, value{};
    std::uint32_t deadline_us{}, settings{};
};
Transfer tx;

OledSettings::Settings decode_settings(std::uint32_t bits) {
    OledSettings::Settings settings;
    settings.brightness=static_cast<std::uint8_t>(bits);
    settings.idle_brightness=static_cast<std::uint8_t>(bits>>8);
    settings.dim_index=static_cast<std::uint8_t>((bits>>16)&15u);
    settings.off_index=static_cast<std::uint8_t>((bits>>20)&15u);
    settings.enabled=(bits&(1u<<24))!=0;
    settings.invert=(bits&(1u<<25))!=0;
    settings.flip=(bits&(1u<<26))!=0;
    settings.fade=(bits&(1u<<27))!=0;
    return settings;
}

TxResult advance_transfer() {
    auto* hw=i2c_get_hw(i2c1);
    const auto flags=hw->raw_intr_stat;
    if ((flags&I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) || hw->tx_abrt_source) {
        (void)hw->clr_tx_abrt;
        return TxResult::Failed;
    }
    if (tx.queued==tx.size && (flags&I2C_IC_RAW_INTR_STAT_STOP_DET_BITS)) {
        (void)hw->clr_stop_det;
        return TxResult::Done;
    }
    // At most one FIFO's worth of register writes per service visit. A packet
    // may need a later visit to queue its STOP byte; allow 2.5 ms without any
    // FIFO progress, rather than spending that interval waiting on Core0.
    const auto available=i2c_get_write_available(i2c1);
    const unsigned budget=available<16u?available:16u;
    const auto before=tx.queued;
    for (unsigned i=0; i<budget && tx.queued<tx.size; ++i) {
        const unsigned index=tx.queued++;
        hw->data_cmd=tx.packet[index] |
            (tx.queued==tx.size?I2C_IC_DATA_CMD_STOP_BITS:0u);
    }
    const auto now=time_us_32();
    if (tx.queued!=before) tx.deadline_us=now+2500u;
    else if (due(now,tx.deadline_us)) return TxResult::Failed;
    return TxResult::Pending;
}

void start_transfer(TxKind kind,const std::uint8_t* packet,std::size_t size,
                    std::uint8_t value=0,std::uint32_t settings=0) {
    // All callers supply fixed command/data packets no larger than 32 bytes.
    tx.kind=kind;
    tx.size=static_cast<std::uint8_t>(size);
    tx.queued=0;
    tx.value=value;
    tx.settings=settings;
    std::memcpy(tx.packet,packet,size);
    auto* hw=i2c_get_hw(i2c1);
    hw->enable=0;
    hw->tar=DC_OGX_OLED_ADDRESS;
    (void)hw->clr_tx_abrt;
    (void)hw->clr_stop_det;
    hw->enable=1;
    tx.deadline_us=time_us_32()+2500u;
    // Return immediately after filling available FIFO space. Completion and
    // controller state changes are handled on a subsequent service visit.
    (void)advance_transfer();
}

void command(TxKind kind,const std::uint8_t* data,std::size_t size,
             std::uint8_t value=0,std::uint32_t settings=0) {
    std::uint8_t packet[32]{};
    packet[0]=0x00; // SSD1306 command-stream control byte.
    std::memcpy(packet+1,data,size);
    start_transfer(kind,packet,size+1,value,settings);
}
void send_contrast(std::uint8_t brightness) {
    const std::uint8_t packet[]={0x81,brightness};
    command(TxKind::Contrast,packet,sizeof(packet),brightness);
}
void pixel(int x, int y, bool on = true) {
    if (x < 0 || y < 0 || x >= static_cast<int>(kWidth) || y >= static_cast<int>(kHeight)) return;
    const std::size_t pos = static_cast<std::size_t>(y / 8) * kWidth + static_cast<unsigned>(x);
    const auto mask = static_cast<std::uint8_t>(1u << (y & 7));
    if (on) fb[pos] |= mask;
    else fb[pos] &= static_cast<std::uint8_t>(~mask);
}
void line(int x0, int y0, int x1, int y1) {
    int dx = x0 < x1 ? x1-x0 : x0-x1;
    int dy = y0 < y1 ? y1-y0 : y0-y1;
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (true) {
        pixel(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}
void fill_rect(int x, int y, int w, int h, bool on = true) {
    for (int yy = y; yy < y+h; ++yy)
        for (int xx = x; xx < x+w; ++xx) pixel(xx, yy, on);
}
void glyph16(OledGlyphs::Icon symbol, int x, int y, bool on = true) {
    const auto& icon = OledGlyphs::get(symbol);
    for (unsigned row = 0; row < OledGlyphs::kHeight; ++row)
        for (unsigned bit = 0; bit < OledGlyphs::kWidth; ++bit)
            if (icon.rows[row] & (1u << (15u - bit)))
                pixel(x + static_cast<int>(bit), y + static_cast<int>(row), on);
}
struct FontGlyph {
    char ch;
    std::uint8_t advance;
    std::uint8_t left;
    std::uint8_t top;
    std::uint8_t width;
    std::uint8_t height;
    std::uint32_t rows[32];
};
// Rasterized uppercase glyphs from DejaVu Sans Condensed Bold (font14).
static const FontGlyph font14[] = {
    {' ', 4, 0, 0, 0, 0, {}},
    {'0', 9, 0, 0, 9, 12, {0x00000000, 0x0000007c, 0x000000fe, 0x000000e6, 0x000000c6, 0x000000c6, 0x000000c6, 0x000000c6, 0x000000e6, 0x0000007e, 0x0000007c, 0x00000000}},
    {'1', 9, 1, 0, 7, 11, {0x00000000, 0x0000001f, 0x0000001f, 0x0000001c, 0x0000001c, 0x0000001c, 0x0000001c, 0x0000001c, 0x0000001c, 0x0000007e, 0x0000007f}},
    {'2', 9, 1, 0, 7, 11, {0x00000000, 0x0000003f, 0x0000003f, 0x00000070, 0x00000070, 0x00000038, 0x00000018, 0x0000000c, 0x00000007, 0x0000007f, 0x0000007f}},
    {'3', 9, 0, 0, 8, 12, {0x00000000, 0x0000007e, 0x0000007e, 0x000000e0, 0x00000060, 0x0000003c, 0x00000078, 0x000000e0, 0x000000e0, 0x000000fe, 0x0000007e, 0x00000000}},
    {'4', 9, 0, 0, 9, 11, {0x00000000, 0x00000070, 0x00000078, 0x00000078, 0x0000006c, 0x00000066, 0x00000066, 0x000000ff, 0x000000ff, 0x00000070, 0x00000060}},
    {'5', 9, 0, 0, 8, 12, {0x00000000, 0x0000007e, 0x0000007e, 0x00000006, 0x0000003e, 0x0000007e, 0x000000e0, 0x000000e0, 0x000000e0, 0x000000fe, 0x0000007e, 0x00000000}},
    {'6', 9, 0, 0, 9, 12, {0x00000000, 0x000000fc, 0x0000005c, 0x0000000e, 0x0000003e, 0x0000007e, 0x000000ee, 0x000000c6, 0x000000c6, 0x000000fe, 0x0000007c, 0x00000000}},
    {'7', 9, 0, 0, 8, 11, {0x00000000, 0x000000fe, 0x000000fe, 0x00000060, 0x00000070, 0x00000070, 0x00000030, 0x00000038, 0x00000018, 0x0000001c, 0x0000000c}},
    {'8', 9, 0, 0, 8, 12, {0x00000000, 0x0000007c, 0x000000ee, 0x000000e6, 0x000000ee, 0x0000007c, 0x000000fe, 0x000000c6, 0x000000c6, 0x000000fe, 0x0000007c, 0x00000000}},
    {'9', 9, 0, 0, 8, 12, {0x00000000, 0x0000007c, 0x0000006e, 0x000000e6, 0x000000e6, 0x000000ee, 0x000000fe, 0x000000f8, 0x000000e0, 0x0000007e, 0x0000003e, 0x00000000}},
    {'A', 10, 0, 0, 10, 11, {0x00000000, 0x00000078, 0x00000078, 0x0000007c, 0x000000fc, 0x000000cc, 0x000000ce, 0x000001fe, 0x000001fe, 0x00000187, 0x00000387}},
    {'B', 10, 1, 0, 8, 11, {0x00000000, 0x0000007f, 0x0000007f, 0x00000067, 0x00000077, 0x0000007f, 0x0000007f, 0x000000e7, 0x000000e7, 0x0000007f, 0x0000003f}},
    {'C', 9, 0, 0, 9, 12, {0x00000000, 0x000000f8, 0x000001fc, 0x0000000e, 0x00000006, 0x00000006, 0x00000006, 0x00000006, 0x0000000e, 0x000001fc, 0x000000f8, 0x00000000}},
    {'D', 10, 1, 0, 9, 11, {0x00000000, 0x0000007f, 0x000000ff, 0x000000e7, 0x000001c7, 0x000001c7, 0x000001c7, 0x000001c7, 0x000000e7, 0x000000ff, 0x0000003f}},
    {'E', 9, 1, 0, 7, 11, {0x00000000, 0x0000007f, 0x0000003f, 0x00000007, 0x00000007, 0x0000003f, 0x0000003f, 0x00000007, 0x00000007, 0x0000007f, 0x0000007f}},
    {'F', 9, 1, 0, 7, 11, {0x00000000, 0x0000007f, 0x0000003f, 0x00000007, 0x00000007, 0x0000003f, 0x0000003f, 0x00000007, 0x00000007, 0x00000007, 0x00000007}},
    {'G', 10, 0, 0, 10, 12, {0x00000000, 0x000001f8, 0x000001fc, 0x0000000e, 0x00000006, 0x000001c6, 0x000001c6, 0x00000186, 0x0000018e, 0x000001fc, 0x000001f8, 0x00000000}},
    {'H', 11, 1, 0, 9, 11, {0x00000000, 0x000000c7, 0x000000c7, 0x000000c7, 0x000000c7, 0x000000ff, 0x000000ff, 0x000000c7, 0x000000c7, 0x000000c7, 0x000000c7}},
    {'I', 5, 1, 0, 3, 11, {0x00000000, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007}},
    {'J', 5, 0, 0, 4, 14, {0x00000000, 0x0000000e, 0x0000000e, 0x0000000e, 0x0000000e, 0x0000000e, 0x0000000e, 0x0000000e, 0x0000000e, 0x0000000e, 0x0000000e, 0x00000007, 0x00000007, 0x00000003}},
    {'K', 10, 1, 0, 10, 11, {0x00000000, 0x000000e7, 0x00000077, 0x0000003f, 0x0000001f, 0x0000001f, 0x0000001f, 0x0000003f, 0x00000077, 0x000000e7, 0x000001c7}},
    {'L', 8, 1, 0, 7, 11, {0x00000000, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x0000007f, 0x0000007f}},
    {'M', 13, 1, 0, 11, 11, {0x00000000, 0x0000038f, 0x0000038f, 0x000003cf, 0x000003df, 0x0000037b, 0x0000037b, 0x00000373, 0x00000333, 0x00000303, 0x00000303}},
    {'N', 11, 1, 0, 9, 11, {0x00000000, 0x000000c7, 0x000000cf, 0x000000cf, 0x000000df, 0x000000db, 0x000000f3, 0x000000f3, 0x000000f3, 0x000000e3, 0x000000e3}},
    {'O', 11, 0, 0, 11, 12, {0x00000000, 0x000000fc, 0x000001fe, 0x0000038e, 0x00000386, 0x00000306, 0x00000306, 0x00000386, 0x0000038e, 0x000001fc, 0x000000f8, 0x00000000}},
    {'P', 9, 1, 0, 8, 11, {0x00000000, 0x0000007f, 0x0000007f, 0x000000e7, 0x000000e7, 0x000000ff, 0x0000007f, 0x0000000f, 0x00000007, 0x00000007, 0x00000007}},
    {'Q', 11, 0, 0, 11, 14, {0x00000000, 0x000000fc, 0x000001fe, 0x0000038e, 0x00000386, 0x00000306, 0x00000306, 0x00000386, 0x0000038e, 0x000001fc, 0x000000f8, 0x000000c0, 0x00000180, 0x00000000}},
    {'R', 10, 1, 0, 9, 11, {0x00000000, 0x0000003f, 0x0000007f, 0x00000067, 0x00000067, 0x0000007f, 0x0000003f, 0x00000077, 0x00000067, 0x000000e7, 0x000000c7}},
    {'S', 9, 0, 0, 9, 12, {0x00000000, 0x000000fc, 0x000000fe, 0x00000006, 0x0000000e, 0x0000007e, 0x000000f8, 0x000000e0, 0x000000c2, 0x000000fe, 0x0000007e, 0x00000000}},
    {'T', 9, 0, 0, 9, 11, {0x00000000, 0x000001ff, 0x000000ff, 0x00000038, 0x00000038, 0x00000038, 0x00000038, 0x00000038, 0x00000038, 0x00000038, 0x00000038}},
    {'U', 10, 1, 0, 9, 12, {0x00000000, 0x000000c7, 0x000000c7, 0x000000c7, 0x000000c7, 0x000000c7, 0x000000c7, 0x000000c7, 0x000000e7, 0x000000fe, 0x0000007e, 0x00000000}},
    {'V', 10, 0, 0, 10, 11, {0x00000000, 0x00000387, 0x00000186, 0x000001c6, 0x000001ce, 0x000000cc, 0x000000ec, 0x000000fc, 0x00000078, 0x00000078, 0x00000078}},
    {'W', 14, 0, 0, 14, 11, {0x00000000, 0x000018c7, 0x000019e6, 0x000019e6, 0x00001dee, 0x00001dee, 0x00000d2c, 0x00000f3c, 0x00000f3c, 0x00000f3c, 0x00000f1c}},
    {'X', 10, 0, 0, 10, 11, {0x00000000, 0x000001c6, 0x000000ce, 0x000000fc, 0x00000078, 0x00000078, 0x00000078, 0x0000007c, 0x000000ec, 0x000001ce, 0x00000187}},
    {'Y', 9, 0, 0, 10, 11, {0x00000000, 0x000001c7, 0x000000ce, 0x000000ee, 0x0000007c, 0x00000078, 0x00000038, 0x00000038, 0x00000038, 0x00000038, 0x00000038}},
    {'Z', 9, 0, 0, 9, 11, {0x00000000, 0x000000fe, 0x000000fe, 0x000000e0, 0x00000070, 0x00000038, 0x00000018, 0x0000001c, 0x0000000e, 0x000001fe, 0x000001ff}},
    {'.', 5, 1, 8, 3, 3, {0x00000003, 0x00000007, 0x00000007}},
    {'-', 5, 0, 5, 5, 3, {0x00000000, 0x0000001e, 0x0000001e}},
    {':', 5, 1, 3, 3, 8, {0x00000002, 0x00000007, 0x00000007, 0x00000000, 0x00000000, 0x00000002, 0x00000007, 0x00000007}},
    {'/', 5, 0, 0, 5, 13, {0x00000000, 0x00000008, 0x00000008, 0x0000000c, 0x0000000c, 0x00000004, 0x00000004, 0x00000006, 0x00000002, 0x00000002, 0x00000003, 0x00000003, 0x00000000}},
    {'_', 6, 0, 13, 7, 2, {0x0000003f, 0x00000000}},
};
// Rasterized uppercase glyphs from DejaVu Sans Condensed Bold (font18).
static const FontGlyph font18[] = {
    {' ', 6, 0, 0, 0, 0, {}},
    {'0', 11, 0, 0, 11, 15, {0x00000000, 0x000000f8, 0x000001fc, 0x0000039e, 0x0000038e, 0x0000038e, 0x0000078e, 0x0000078e, 0x0000078e, 0x0000038e, 0x0000038e, 0x0000039e, 0x000001fc, 0x000000f8, 0x00000000}},
    {'1', 11, 1, 0, 10, 14, {0x00000000, 0x0000007e, 0x0000007e, 0x0000007e, 0x00000070, 0x00000070, 0x00000070, 0x00000070, 0x00000070, 0x00000070, 0x00000070, 0x00000078, 0x000001fe, 0x000001fe}},
    {'2', 11, 1, 0, 9, 14, {0x00000000, 0x0000007f, 0x000000ff, 0x000001e3, 0x000001c0, 0x000001c0, 0x000000e0, 0x000000f0, 0x00000078, 0x0000003c, 0x0000001e, 0x000000ff, 0x000001ff, 0x000001ff}},
    {'3', 11, 1, 0, 9, 15, {0x00000000, 0x000000fe, 0x000000fe, 0x000001e0, 0x000001c0, 0x000000e0, 0x000000fc, 0x000000fc, 0x000001f0, 0x000001c0, 0x000001c0, 0x000001e1, 0x000000ff, 0x0000007f, 0x00000000}},
    {'4', 11, 0, 0, 11, 14, {0x00000000, 0x000001e0, 0x000001f0, 0x000001f0, 0x000001f8, 0x000001d8, 0x000001cc, 0x000001ce, 0x000001c6, 0x000007fe, 0x000007fe, 0x000003fe, 0x000001c0, 0x000001c0}},
    {'5', 11, 1, 0, 10, 15, {0x00000000, 0x000000fe, 0x000000fe, 0x0000000e, 0x00000006, 0x0000007e, 0x000000fe, 0x000001fe, 0x000001c0, 0x000001c0, 0x000001c0, 0x000001e3, 0x000000ff, 0x0000007e, 0x00000000}},
    {'6', 11, 1, 0, 10, 15, {0x00000000, 0x000001f8, 0x000001fe, 0x0000000e, 0x00000007, 0x00000077, 0x000000ff, 0x000001ff, 0x000001cf, 0x000001c7, 0x000001c7, 0x000001ce, 0x000001fe, 0x000000fc, 0x00000000}},
    {'7', 11, 1, 0, 9, 14, {0x00000000, 0x000001ff, 0x000001ff, 0x000001e0, 0x000000e0, 0x000000e0, 0x000000f0, 0x00000070, 0x00000070, 0x00000038, 0x00000038, 0x0000001c, 0x0000001c, 0x0000001c}},
    {'8', 11, 1, 0, 10, 15, {0x00000000, 0x000000fe, 0x000001fe, 0x000001c7, 0x000001c7, 0x000001ce, 0x000000fe, 0x000000fe, 0x000001cf, 0x000001c7, 0x000001c7, 0x000001c7, 0x000001ff, 0x000000fe, 0x00000000}},
    {'9', 11, 0, 0, 11, 15, {0x00000000, 0x000000f8, 0x000001fc, 0x0000038e, 0x0000038e, 0x0000038e, 0x0000038e, 0x000003fe, 0x000003fc, 0x000003b0, 0x00000380, 0x000001c0, 0x000001fc, 0x000000fc, 0x00000000}},
    {'A', 13, 0, 0, 13, 14, {0x00000000, 0x000000f0, 0x000001f0, 0x000001f0, 0x000001f8, 0x000003b8, 0x000003b8, 0x0000039c, 0x0000071c, 0x000007fc, 0x000007fe, 0x00000ffe, 0x00000e0e, 0x00000e07}},
    {'B', 12, 1, 0, 11, 14, {0x00000000, 0x000000ff, 0x000001ff, 0x000003cf, 0x0000038f, 0x000003cf, 0x000001ff, 0x000001ff, 0x000003cf, 0x0000038f, 0x0000038f, 0x000003cf, 0x000003ff, 0x000000ff}},
    {'C', 12, 0, 0, 11, 15, {0x00000000, 0x000007f0, 0x000007fc, 0x0000063c, 0x0000001e, 0x0000000e, 0x0000000e, 0x0000000e, 0x0000000e, 0x0000000e, 0x0000001e, 0x0000063c, 0x000007f8, 0x000003f0, 0x00000000}},
    {'D', 13, 1, 0, 12, 14, {0x00000000, 0x000000ff, 0x000003ff, 0x000007ff, 0x0000078f, 0x0000070f, 0x00000f0f, 0x00000f0f, 0x00000f0f, 0x0000070f, 0x0000078f, 0x000003ff, 0x000003ff, 0x000000ff}},
    {'E', 11, 1, 0, 9, 14, {0x00000000, 0x000001ff, 0x000001ff, 0x000000ff, 0x0000000f, 0x0000000f, 0x000000ff, 0x000000ff, 0x000000ff, 0x0000000f, 0x0000000f, 0x000001ff, 0x000001ff, 0x000001ff}},
    {'F', 11, 1, 0, 9, 14, {0x00000000, 0x000001ff, 0x000001ff, 0x000000ff, 0x0000000f, 0x0000000f, 0x000000ff, 0x000000ff, 0x000000ff, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f}},
    {'G', 13, 0, 0, 13, 15, {0x00000000, 0x000007f0, 0x00000ff8, 0x00000c3c, 0x0000001e, 0x0000000e, 0x0000000e, 0x00000f8e, 0x00000f8e, 0x00000e0e, 0x00000e1e, 0x00000e3c, 0x00000ff8, 0x000007f0, 0x00000000}},
    {'H', 14, 1, 0, 12, 14, {0x00000000, 0x0000070f, 0x0000070f, 0x0000070f, 0x0000070f, 0x0000070f, 0x000007ff, 0x000007ff, 0x000007ff, 0x0000070f, 0x0000070f, 0x0000070f, 0x0000070f, 0x0000070f}},
    {'I', 6, 1, 0, 4, 14, {0x00000000, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f}},
    {'J', 6, 0, 0, 5, 18, {0x00000000, 0x0000001e, 0x0000001e, 0x0000001e, 0x0000001e, 0x0000001e, 0x0000001e, 0x0000001e, 0x0000001e, 0x0000001e, 0x0000001e, 0x0000001e, 0x0000001e, 0x0000001e, 0x0000000e, 0x0000000f, 0x00000007, 0x00000001}},
    {'K', 13, 1, 0, 12, 14, {0x00000000, 0x0000078f, 0x000003cf, 0x000001ef, 0x000000ef, 0x0000007f, 0x0000003f, 0x0000003f, 0x0000007f, 0x000000ff, 0x000001ef, 0x000003cf, 0x0000078f, 0x00000f0f}},
    {'L', 10, 1, 0, 9, 14, {0x00000000, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x000001ff, 0x000001ff, 0x000001ff}},
    {'M', 16, 1, 0, 14, 14, {0x00000000, 0x00003e1f, 0x00003e1f, 0x00003e1f, 0x00003f3f, 0x00003f3f, 0x00003b77, 0x00003bf7, 0x000039e7, 0x000039e7, 0x000038c7, 0x000038c7, 0x00003807, 0x00003807}},
    {'N', 14, 1, 0, 12, 14, {0x00000000, 0x0000070f, 0x0000071f, 0x0000071f, 0x0000073f, 0x0000073f, 0x00000777, 0x00000777, 0x000007e7, 0x000007e7, 0x000007c7, 0x000007c7, 0x00000787, 0x00000787}},
    {'O', 14, 0, 0, 13, 15, {0x00000000, 0x000003f0, 0x000007fc, 0x00000f3c, 0x00001e1e, 0x00001e0e, 0x00001c0e, 0x00001c0e, 0x00001c0e, 0x00001e0e, 0x00001e1e, 0x00000f3c, 0x000007fc, 0x000003f0, 0x00000000}},
    {'P', 12, 1, 0, 11, 14, {0x00000000, 0x000000ff, 0x000001ff, 0x000003cf, 0x0000038f, 0x0000038f, 0x000003cf, 0x000003ff, 0x000001ff, 0x0000003f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f}},
    {'Q', 14, 0, 0, 13, 17, {0x00000000, 0x000003f0, 0x000007fc, 0x00000f3c, 0x00001e1e, 0x00001e0e, 0x00001c0e, 0x00001c0e, 0x00001c0e, 0x00001e0e, 0x00000e1e, 0x00000f3c, 0x000007fc, 0x000003f0, 0x00000380, 0x00000700, 0x00000e00}},
    {'R', 12, 1, 0, 12, 14, {0x00000000, 0x000000ff, 0x000001ff, 0x000003ef, 0x000003cf, 0x000003cf, 0x000001cf, 0x000000ff, 0x000000ff, 0x000001cf, 0x000003cf, 0x0000038f, 0x0000078f, 0x0000070f}},
    {'S', 12, 1, 0, 10, 15, {0x00000000, 0x000001fc, 0x000001fe, 0x0000018f, 0x00000007, 0x0000000f, 0x0000007e, 0x000001fe, 0x000001f8, 0x000001c0, 0x000003c0, 0x000001c7, 0x000001ff, 0x000000fe, 0x00000000}},
    {'T', 11, 0, 0, 11, 14, {0x00000000, 0x000007ff, 0x000007ff, 0x000003fe, 0x00000070, 0x00000070, 0x00000070, 0x00000070, 0x00000070, 0x00000070, 0x00000070, 0x00000070, 0x00000070, 0x00000070}},
    {'U', 13, 1, 0, 11, 15, {0x00000000, 0x0000070f, 0x0000070f, 0x0000070f, 0x0000070f, 0x0000070f, 0x0000070f, 0x0000070f, 0x0000070f, 0x0000078f, 0x0000078e, 0x000003de, 0x000003fe, 0x000001fc, 0x00000000}},
    {'V', 13, 0, 0, 13, 14, {0x00000000, 0x00000e07, 0x00000e0e, 0x00000f0e, 0x0000070e, 0x0000071c, 0x0000071c, 0x0000039c, 0x000003b8, 0x000003b8, 0x000001f8, 0x000001f0, 0x000001f0, 0x000000f0}},
    {'W', 18, 0, 0, 18, 14, {0x00000000, 0x0001c78e, 0x0001c78e, 0x0001c78e, 0x0001c78e, 0x0000e7ce, 0x0000eedc, 0x0000ecdc, 0x0000ecdc, 0x0000fcfc, 0x00007cfc, 0x00007c78, 0x00007878, 0x00007878}},
    {'X', 12, 0, 0, 13, 14, {0x00000000, 0x00000f0e, 0x0000071e, 0x0000039c, 0x000003b8, 0x000001f8, 0x000001f0, 0x000000f0, 0x000001f0, 0x000001f8, 0x000003bc, 0x0000079c, 0x0000070e, 0x00000e0e}},
    {'Y', 12, 0, 0, 12, 14, {0x00000000, 0x00000f0f, 0x0000070e, 0x0000039e, 0x000003dc, 0x000001f8, 0x000001f8, 0x000000f0, 0x000000f0, 0x00000070, 0x00000070, 0x00000070, 0x00000070, 0x00000070}},
    {'Z', 12, 0, 0, 12, 14, {0x00000000, 0x000007fe, 0x000007fe, 0x000003fe, 0x000003c0, 0x000001e0, 0x000000e0, 0x000000f0, 0x00000078, 0x0000003c, 0x0000001c, 0x000007fe, 0x000007fe, 0x000007fe}},
    {'.', 6, 1, 10, 4, 4, {0x00000000, 0x0000000e, 0x0000000e, 0x0000000e}},
    {'-', 7, 0, 7, 6, 4, {0x0000001e, 0x0000003e, 0x0000003e, 0x00000000}},
    {':', 6, 1, 4, 4, 10, {0x0000000e, 0x0000000e, 0x0000000e, 0x00000006, 0x00000000, 0x00000000, 0x00000000, 0x0000000e, 0x0000000e, 0x0000000e}},
    {'/', 6, 0, 0, 6, 16, {0x00000000, 0x00000030, 0x00000030, 0x00000018, 0x00000018, 0x00000018, 0x00000008, 0x0000000c, 0x0000000c, 0x0000000c, 0x00000006, 0x00000006, 0x00000006, 0x00000002, 0x00000003, 0x00000003}},
    {'_', 8, 0, 16, 9, 3, {0x00000000, 0x000000ff, 0x00000000}},
};
// Rasterized uppercase glyphs from DejaVu Sans Condensed Bold (font25).
static const FontGlyph font25[] = {
    {' ', 8, 0, 0, 0, 0, {}},
    {'0', 16, 1, 0, 14, 20, {0x000000c0, 0x000003f8, 0x00000ffc, 0x00000ffe, 0x00001f1e, 0x00001e1e, 0x00001e1f, 0x00001e0f, 0x00003e0f, 0x00003e0f, 0x00003e0f, 0x00003e0f, 0x00001e1f, 0x00001e1f, 0x00001e1e, 0x00001f1e, 0x00000ffc, 0x000007fc, 0x000003f8, 0x00000000}},
    {'1', 16, 2, 0, 13, 19, {0x00000000, 0x000000fe, 0x000000ff, 0x000000ff, 0x000000f2, 0x000000f0, 0x000000f0, 0x000000f0, 0x000000f0, 0x000000f0, 0x000000f0, 0x000000f0, 0x000000f0, 0x000000f0, 0x000000f0, 0x000001f0, 0x00000ffe, 0x00000ffe, 0x00000ffe}},
    {'2', 16, 1, 0, 13, 19, {0x000000e0, 0x000003fe, 0x00000ffe, 0x00000ffe, 0x00000f86, 0x00001f00, 0x00001f00, 0x00000f00, 0x00000f00, 0x00000780, 0x000007c0, 0x000003e0, 0x000001f0, 0x000000f8, 0x0000007c, 0x00000ffe, 0x00001ffe, 0x00001ffe, 0x00001ffe}},
    {'3', 16, 1, 0, 13, 20, {0x000000e0, 0x000007fe, 0x00000ffe, 0x00000ffe, 0x00000f00, 0x00001f00, 0x00000f00, 0x00000f80, 0x000007f8, 0x000003f8, 0x00000ff8, 0x00000f80, 0x00001f00, 0x00001f00, 0x00001f00, 0x00001f03, 0x00000fff, 0x00000fff, 0x000003fe, 0x00000000}},
    {'4', 16, 1, 0, 14, 19, {0x00000000, 0x000007c0, 0x000007e0, 0x000007e0, 0x000007f0, 0x000007f0, 0x000007b8, 0x000007bc, 0x0000079c, 0x0000078e, 0x0000078e, 0x00000787, 0x00003fff, 0x00003fff, 0x00003fff, 0x00001fff, 0x00000780, 0x00000780, 0x00000780}},
    {'5', 16, 1, 0, 14, 20, {0x00000000, 0x00000ffe, 0x00000ffe, 0x00000ffe, 0x0000001e, 0x0000001e, 0x0000001e, 0x000003fe, 0x00000ffe, 0x00000ffe, 0x00001f86, 0x00001f00, 0x00001e00, 0x00001e00, 0x00001f00, 0x00001f06, 0x00000ffe, 0x000007fe, 0x000003fe, 0x00000000}},
    {'6', 16, 1, 0, 14, 20, {0x00000180, 0x00000ff0, 0x00000ff8, 0x00000ffc, 0x0000003e, 0x0000001e, 0x0000001e, 0x000003fe, 0x00000fff, 0x00001fff, 0x00001f3f, 0x00001e1f, 0x00001e1f, 0x00001e1e, 0x00001e1e, 0x00001e3e, 0x00001ffc, 0x00000ff8, 0x000007f0, 0x00000000}},
    {'7', 16, 1, 0, 13, 19, {0x00000000, 0x00001fff, 0x00001fff, 0x00001fff, 0x00001f00, 0x00000f00, 0x00000f80, 0x00000780, 0x00000780, 0x000003c0, 0x000003c0, 0x000003e0, 0x000001e0, 0x000001e0, 0x000000f0, 0x000000f0, 0x000000f8, 0x00000078, 0x0000007c}},
    {'8', 16, 1, 0, 14, 20, {0x000001e0, 0x000007fc, 0x00000ffe, 0x00001ffe, 0x00001e1e, 0x00001e1e, 0x00001e1e, 0x00000f3e, 0x000007fc, 0x000007f8, 0x00000ffc, 0x00001f1e, 0x00001e1f, 0x00001e0f, 0x00001e1f, 0x00001e1e, 0x00001ffe, 0x00000ffc, 0x000007f8, 0x00000000}},
    {'9', 16, 1, 0, 14, 20, {0x000000e0, 0x000003f8, 0x000007fc, 0x00000ffe, 0x00000f1f, 0x00001e0f, 0x00001e0f, 0x00001e0f, 0x00001f1f, 0x00001fbe, 0x00001ffe, 0x00001ffc, 0x00001ef8, 0x00001e00, 0x00000f00, 0x00000f82, 0x000007fe, 0x000007fe, 0x000001fe, 0x00000000}},
    {'A', 17, 0, 0, 18, 19, {0x00000000, 0x00000fc0, 0x00000fc0, 0x00000fe0, 0x00001fe0, 0x00001fe0, 0x00001ef0, 0x00003ef0, 0x00003cf0, 0x00003c78, 0x00007c78, 0x00007878, 0x00007ffc, 0x0000fffc, 0x0000fffc, 0x0000fffe, 0x0001f01e, 0x0001e01e, 0x0001e01f}},
    {'B', 17, 2, 0, 14, 19, {0x00000000, 0x000007ff, 0x00000fff, 0x00001fff, 0x00001f0f, 0x00001e0f, 0x00001f0f, 0x00001f0f, 0x00000fff, 0x000007ff, 0x00001fff, 0x00001f0f, 0x00001e0f, 0x00003e0f, 0x00003e0f, 0x00001f1f, 0x00001fff, 0x00000fff, 0x000007ff}},
    {'C', 16, 1, 0, 15, 20, {0x00000780, 0x00003ff0, 0x00003ff8, 0x00003ffc, 0x0000307e, 0x0000003e, 0x0000001e, 0x0000001f, 0x0000001f, 0x0000001f, 0x0000001f, 0x0000001f, 0x0000001f, 0x0000001e, 0x0000003e, 0x0000307e, 0x00003ffc, 0x00003ff8, 0x00001fe0, 0x00000000}},
    {'D', 19, 2, 0, 16, 19, {0x00000000, 0x000007ff, 0x00000fff, 0x00001fff, 0x00003f9f, 0x00007e0f, 0x00007c0f, 0x0000780f, 0x0000780f, 0x0000f80f, 0x0000f80f, 0x0000780f, 0x0000780f, 0x00007c0f, 0x00007e0f, 0x00003fff, 0x00001fff, 0x00000fff, 0x000003ff}},
    {'E', 15, 2, 0, 12, 19, {0x00000000, 0x00000fff, 0x00000fff, 0x00000fff, 0x0000001f, 0x0000000f, 0x0000000f, 0x0000001f, 0x000007ff, 0x000007ff, 0x000007ff, 0x0000001f, 0x0000000f, 0x0000000f, 0x0000000f, 0x000007ff, 0x00000fff, 0x00000fff, 0x00000fff}},
    {'F', 15, 2, 0, 12, 19, {0x00000000, 0x00000fff, 0x00000fff, 0x00000fff, 0x0000001f, 0x0000000f, 0x0000000f, 0x0000001f, 0x000007ff, 0x000007ff, 0x000007ff, 0x0000001f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f}},
    {'G', 18, 1, 0, 16, 20, {0x00000700, 0x00007ff0, 0x00007ff8, 0x00007ffc, 0x0000607e, 0x0000003e, 0x0000001e, 0x0000001f, 0x0000001f, 0x0000fe1f, 0x0000fe1f, 0x0000fe1f, 0x0000f01f, 0x0000f01e, 0x0000f03e, 0x0000f8fe, 0x0000fffc, 0x0000fff8, 0x00003fe0, 0x00000000}},
    {'H', 19, 2, 0, 15, 19, {0x00000000, 0x00007c0f, 0x00007c0f, 0x00007c0f, 0x00007c0f, 0x00007c0f, 0x00007c0f, 0x00007c1f, 0x00007fff, 0x00007fff, 0x00007fff, 0x00007c1f, 0x00007c0f, 0x00007c0f, 0x00007c0f, 0x00007c0f, 0x00007c0f, 0x00007c0f, 0x00007c0f}},
    {'I', 8, 2, 0, 5, 19, {0x00000000, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f}},
    {'J', 8, 0, 0, 7, 24, {0x00000000, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003c, 0x0000003e, 0x0000003f, 0x0000001f, 0x0000000f, 0x00000007}},
    {'K', 17, 2, 0, 17, 19, {0x00000000, 0x00007c0f, 0x00003e0f, 0x00001f0f, 0x00000f8f, 0x000007cf, 0x000003ef, 0x000001ff, 0x000000ff, 0x000000ff, 0x000000ff, 0x000001ff, 0x000003ff, 0x000007cf, 0x00000f8f, 0x00001f0f, 0x00003f0f, 0x00007e0f, 0x0000fc0f}},
    {'L', 14, 2, 0, 12, 19, {0x00000000, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x000007ff, 0x00000fff, 0x00000fff, 0x00000fff}},
    {'M', 22, 2, 0, 19, 19, {0x00000000, 0x0003e03f, 0x0003f03f, 0x0003f07f, 0x0003f87f, 0x0003f87f, 0x0003f8ff, 0x0003fcef, 0x0003dcef, 0x0003ddef, 0x0003dfcf, 0x0003cfcf, 0x0003cf8f, 0x0003c78f, 0x0003c78f, 0x0003c30f, 0x0003c00f, 0x0003c00f, 0x0003c00f}},
    {'N', 19, 2, 0, 15, 19, {0x00000000, 0x0000781f, 0x0000783f, 0x0000783f, 0x0000787f, 0x0000787f, 0x000078ff, 0x000078ff, 0x000079ef, 0x000079ef, 0x000079cf, 0x00007bcf, 0x00007f8f, 0x00007f8f, 0x00007f0f, 0x00007f0f, 0x00007e0f, 0x00007e0f, 0x00007c0f}},
    {'O', 19, 1, 0, 17, 20, {0x00000380, 0x00001ff0, 0x00003ff8, 0x00007ffc, 0x0000f87e, 0x0000f83e, 0x0001f01f, 0x0001f01f, 0x0001f01f, 0x0001e01f, 0x0001e01f, 0x0001f01f, 0x0001f01f, 0x0001f01e, 0x0000f83e, 0x0000fc7e, 0x00007ffc, 0x00003ff8, 0x00001ff0, 0x00000000}},
    {'P', 16, 2, 0, 14, 19, {0x00000000, 0x000007ff, 0x00000fff, 0x00001fff, 0x00001f0f, 0x00003e0f, 0x00003e0f, 0x00003e0f, 0x00001f0f, 0x00001fff, 0x00000fff, 0x000007ff, 0x000001ff, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f}},
    {'Q', 19, 1, 0, 17, 23, {0x00000380, 0x00001ff0, 0x00003ff8, 0x00007ffc, 0x0000f87e, 0x0000f83e, 0x0001f01f, 0x0001f01f, 0x0001f01f, 0x0001e01f, 0x0001e01f, 0x0001f01f, 0x0001f01f, 0x0001f01e, 0x0000f83e, 0x0000fc7e, 0x00007ffc, 0x00003ff8, 0x00001fe0, 0x00001e00, 0x00003c00, 0x00007800, 0x00007000}},
    {'R', 17, 2, 0, 15, 19, {0x00000000, 0x000007ff, 0x00000fff, 0x00000fff, 0x00001f8f, 0x00001f0f, 0x00001f0f, 0x00001f0f, 0x00000f8f, 0x000007ff, 0x000003ff, 0x000007ff, 0x00000f8f, 0x00000f0f, 0x00001f0f, 0x00001e0f, 0x00003e0f, 0x00003c0f, 0x00007c0f}},
    {'S', 16, 1, 0, 14, 20, {0x000000e0, 0x00000ff8, 0x00001ffc, 0x00001ffe, 0x0000181e, 0x0000001e, 0x0000001e, 0x0000007e, 0x000003fe, 0x00000ffc, 0x00001ff8, 0x00001fe0, 0x00003e00, 0x00003e00, 0x00003e02, 0x00001f0e, 0x00001ffe, 0x00000ffe, 0x000007fc, 0x00000000}},
    {'T', 15, 0, 0, 16, 19, {0x00000000, 0x00007fff, 0x00007fff, 0x00007fff, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0, 0x000003e0}},
    {'U', 18, 2, 0, 15, 20, {0x00000000, 0x00003c0f, 0x00003c0f, 0x00003c0f, 0x00003c0f, 0x00003c0f, 0x00003c0f, 0x00003c0f, 0x00003c0f, 0x00003c0f, 0x00003c0f, 0x00003c0f, 0x00003c0f, 0x00003c1f, 0x00003e1f, 0x00003f3e, 0x00001ffe, 0x00000ffc, 0x000007f8, 0x00000000}},
    {'V', 17, 0, 0, 18, 19, {0x00000000, 0x0001e01f, 0x0001f01e, 0x0000f01e, 0x0000f03e, 0x0000f83c, 0x0000783c, 0x0000787c, 0x00007c78, 0x00003c78, 0x00003cf8, 0x00003cf0, 0x00001ef0, 0x00001ff0, 0x00001fe0, 0x00000fe0, 0x00000fe0, 0x00000fc0, 0x000007c0}},
    {'W', 25, 0, 0, 25, 19, {0x00000000, 0x00f07c1e, 0x00f07c1e, 0x00f87c1e, 0x00787c3e, 0x0078fe3c, 0x0078ee3c, 0x0078ee3c, 0x007cee3c, 0x003cef7c, 0x003de778, 0x003dc778, 0x003dc778, 0x001fc7f8, 0x001fc3f0, 0x001f83f0, 0x001f83f0, 0x001f83f0, 0x000f83f0}},
    {'X', 17, 0, 0, 17, 19, {0x00000000, 0x0000f03e, 0x0000f83c, 0x0000787c, 0x00003c78, 0x00003ef8, 0x00001ff0, 0x00001fe0, 0x00000fe0, 0x000007c0, 0x00000fc0, 0x00000fe0, 0x00001ff0, 0x00003ef0, 0x00003cf8, 0x00007c7c, 0x0000783c, 0x0000f83e, 0x0001f01e}},
    {'Y', 16, 0, 0, 17, 19, {0x00000000, 0x0000f81f, 0x0000f81e, 0x00007c3e, 0x00003c7c, 0x00003e78, 0x00001ef8, 0x00001ff0, 0x00000ff0, 0x000007e0, 0x000007e0, 0x000003c0, 0x000003c0, 0x000003c0, 0x000003c0, 0x000003c0, 0x000003c0, 0x000003c0, 0x000003c0}},
    {'Z', 16, 1, 0, 15, 19, {0x00000000, 0x00003fff, 0x00003fff, 0x00003fff, 0x00001f00, 0x00001f00, 0x00000f80, 0x000007c0, 0x000003c0, 0x000003e0, 0x000001f0, 0x000000f8, 0x000000f8, 0x0000007c, 0x0000003e, 0x00003fff, 0x00003fff, 0x00003fff, 0x00003fff}},
    {'.', 9, 2, 14, 5, 5, {0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f}},
    {'-', 9, 1, 10, 8, 4, {0x0000007f, 0x0000007f, 0x0000007f, 0x0000007f}},
    {':', 9, 2, 5, 5, 14, {0x0000000e, 0x0000001f, 0x0000001f, 0x0000001f, 0x0000001f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0000000e, 0x0000001f, 0x0000001f, 0x0000001f, 0x0000001f}},
    {'/', 8, 0, 0, 9, 22, {0x00000000, 0x000000c0, 0x000000e0, 0x000000e0, 0x00000060, 0x00000070, 0x00000070, 0x00000030, 0x00000030, 0x00000038, 0x00000038, 0x00000018, 0x0000001c, 0x0000001c, 0x0000000c, 0x0000000c, 0x0000000e, 0x0000000e, 0x00000006, 0x00000007, 0x00000007, 0x00000000}},
    {'_', 11, 0, 22, 12, 3, {0x00000000, 0x000007ff, 0x000007ff}},
};

// Compact original 5x7 pixel font. One byte per scanline, bit4 is leftmost.
// Used sparingly for status tags. Important values always use larger fonts.
struct MicroGlyph { char ch; std::uint8_t r[7]; };
static constexpr MicroGlyph kMicro[] = {
    {' ',{0,0,0,0,0,0,0}},
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{31,4,4,4,4,4,31}},      {'J',{7,2,2,2,18,18,12}},
    {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}},  {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}},     {'Z',{31,1,2,4,8,16,31}},
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}},     {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}},    {'5',{31,16,16,30,1,1,30}},
    {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
    // Unlike status labels, SSIDs are case-sensitive. Draw their exact case.
    {'a',{0,0,14,1,15,17,15}},  {'b',{16,16,30,17,17,17,30}},
    {'c',{0,0,15,16,16,16,15}}, {'d',{1,1,15,17,17,17,15}},
    {'e',{0,0,14,17,31,16,14}}, {'f',{6,9,8,28,8,8,8}},
    {'g',{0,15,17,17,15,1,14}}, {'h',{16,16,30,17,17,17,17}},
    {'i',{4,0,12,4,4,4,14}},    {'j',{2,0,6,2,2,18,12}},
    {'k',{16,16,18,20,24,20,18}}, {'l',{12,4,4,4,4,4,14}},
    {'m',{0,0,26,21,21,21,21}}, {'n',{0,0,30,17,17,17,17}},
    {'o',{0,0,14,17,17,17,14}}, {'p',{0,0,30,17,30,16,16}},
    {'q',{0,0,15,17,15,1,1}},  {'r',{0,0,22,25,16,16,16}},
    {'s',{0,0,15,16,14,1,30}}, {'t',{8,8,30,8,8,9,6}},
    {'u',{0,0,17,17,17,19,13}}, {'v',{0,0,17,17,17,10,4}},
    {'w',{0,0,17,17,21,21,10}}, {'x',{0,0,17,10,4,10,17}},
    {'y',{0,0,17,17,15,1,14}}, {'z',{0,0,31,2,4,8,31}},
    {'/',{1,1,2,4,8,16,16}},     {':',{0,4,4,0,4,4,0}},
    {'.',{0,0,0,0,0,12,12}},      {'-',{0,0,0,31,0,0,0}},
    {'_', {0,0,0,0,0,0,31}},
    {'%',{25,25,2,4,8,19,19}},   {'!',{4,4,4,4,4,0,4}},
    {'?',{14,17,1,2,4,0,4}},     {'+',{0,4,4,31,4,4,0}},
};
const MicroGlyph* micro_lookup(char ch) {
    for (const auto& entry : kMicro) if (entry.ch == ch) return &entry;
    return nullptr;
}
int micro_width(const char* text) {
    return text ? static_cast<int>(std::strlen(text))*6 : 0;
}
void micro_text(int x, int y, const char* text, bool on=true) {
    if (!text) return;
    for (; *text; ++text, x+=6) {
        const auto* glyph = micro_lookup(*text);
        if (!glyph) continue;
        for (int yy=0; yy<7; ++yy)
            for (int xx=0; xx<5; ++xx)
                if (glyph->r[yy] & (1u << (4-xx))) pixel(x+xx,y+yy,on);
    }
}
const FontGlyph* lookup(const FontGlyph* table, std::size_t count, char ch) {
    for (std::size_t i=0; i<count; ++i) if (table[i].ch == ch) return &table[i];
    return nullptr;
}
struct Font { const FontGlyph* table; std::size_t count; };
constexpr Font kCompact{font14, sizeof(font14)/sizeof(font14[0])};
constexpr Font kText{font18, sizeof(font18)/sizeof(font18[0])};
constexpr Font kBig{font25, sizeof(font25)/sizeof(font25[0])};
int string_width(const Font& f, const char* text) {
    int width = 0;
    if (text) for (; *text; ++text)
        if (const FontGlyph* g = lookup(f.table, f.count, *text)) width += g->advance;
    return width;
}
void draw_text(int x, int y, const char* text, const Font& f, bool on = true) {
    if (!text) return;
    for (; *text; ++text) {
        const FontGlyph* g = lookup(f.table,f.count,*text);
        if (!g) continue;
        for (unsigned yy=0; yy<g->height; ++yy)
            for (unsigned xx=0; xx<g->width; ++xx)
                if (g->rows[yy] & (1u << xx))
                    pixel(x+static_cast<int>(g->left+xx),
                          y+static_cast<int>(g->top+yy),on);
        x += g->advance;
    }
}
// IP addresses are kept intact and never rendered in an unreadable tiny font.
bool valid_ipv4(const char* ip) {
    if (!ip || !*ip) return false;
    const char* p=ip;
    for (unsigned octet=0; octet<4; ++octet) {
        unsigned value=0, digits=0;
        while (*p>='0' && *p<='9' && digits<3) {
            value=value*10u+static_cast<unsigned>(*p++-'0'); ++digits;
        }
        if (!digits || value>255 || (*p>='0' && *p<='9')) return false;
        if (octet<3) { if (*p++ != '.') return false; }
        else if (*p) return false;
    }
    return string_width(kCompact,ip) <= 124;
}
// Existing project DXC logo, preserved as native monochrome art.
static const std::uint64_t kBrandLogo64x24[24] = {
    0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0xfffc0fe00fe03ffeULL,
    0xfffe07f01fc07ffeULL, 0xffff03f83f80fffeULL,
    0xffffc3f83f83fffeULL, 0xffffe1fc7f07fffeULL,
    0xfffff0fc7e0fe03eULL, 0x000ff07bbc0fc03eULL,
    0x0007f0739c0f803eULL, 0x0003f02fe80f803eULL,
    0x0003f00fe00f803eULL, 0x0003f02fe80f803eULL,
    0x0007f0739c0f803eULL, 0x000ff07bbc0fc03eULL,
    0xfffff0fc7e0fe03eULL, 0xffffe1fc7f07fffeULL,
    0xffffc3f83f83fffeULL, 0xffff03f83f80fffeULL,
    0xfffe07f01fc07ffeULL, 0xfffc0fe00fe03ffeULL,
    0x0000000000000000ULL, 0x0000000000000000ULL,
};
void sparkle(int x, int y, std::uint8_t motion) {
    pixel(x,y);
    if ((motion & 3u) < 2u) {
        pixel(x-2,y); pixel(x+2,y); pixel(x,y-2); pixel(x,y+2);
    } else { pixel(x-1,y-1); pixel(x+1,y+1); }
}
void draw_splash(std::uint32_t now) {
    const std::uint32_t elapsed = now - splash_since;
    if (elapsed < 170) {
        // Scanning line / first-pixel ignition; 0.91" friendly and bounded.
        const int sweep=static_cast<int>((elapsed/36u)*24u);
        line(5,16,sweep<123?sweep:123,16);
        sparkle(11,8,static_cast<std::uint8_t>(elapsed/45u));
        return;
    }
    const unsigned reveal = elapsed >= 570 ? 64u : static_cast<unsigned>((elapsed-170u)*64u/400u);
    for (int y=0; y<24; ++y) {
        const std::uint64_t mask=kBrandLogo64x24[y];
        for (unsigned x=0; x<reveal; ++x)
            if ((mask>>x)&1ULL) pixel(2+static_cast<int>(x), 4+y);
    }
    if (elapsed>=470) {
        draw_text(69,1,"OGX-W",kCompact);
    }
    const int scan_x=2+static_cast<int>(reveal);
    if (elapsed<570) line(scan_x,5,scan_x,25);
    if (elapsed>=570) {
        line(68,15,124,15);
        sparkle(124,2,static_cast<std::uint8_t>(elapsed/180));
    }
}
void tiny_wifi(int x,int y,bool active,bool joining,std::uint8_t motion) {
    // Sparse arcs have much better visual contrast than a 32x32 Wi-Fi bitmap.
    const int ring = joining ? static_cast<int>(motion%3u) : active ? 3 : 1;
    if (ring >= 2) {
        pixel(x+0,y+2); pixel(x+1,y+1); pixel(x+2,y+0); pixel(x+3,y+0);
        pixel(x+4,y+0); pixel(x+5,y+1); pixel(x+6,y+2);
    }
    if (ring >= 1) {
        pixel(x+2,y+4); pixel(x+3,y+3); pixel(x+4,y+4);
    }
    pixel(x+3,y+6);
}
// Full SSID including the unique device suffix; never substitute a fixed name.
// Keep the existing Wi-Fi glyph visible; two 16-character rows fit a full SSID.
void draw_ap_ssid(const char* ssid) {
    if (!ssid || !*ssid) return;
    constexpr std::size_t kCharsPerRow = 16;
    glyph16(OledGlyphs::Icon::Wifi,2,15);
    const std::size_t length = std::strlen(ssid);
    if (length <= kCharsPerRow) {
        micro_text(47,15,"JOIN WI-FI");
        micro_text(23+(104-static_cast<int>(length*6))/2,24,ssid);
    } else {
        char top[kCharsPerRow+1]{};
        std::memcpy(top,ssid,kCharsPerRow);
        micro_text(23+(104-static_cast<int>(kCharsPerRow*6))/2,15,top);
        micro_text(23+(104-static_cast<int>((length-kCharsPerRow)*6))/2,
                   24,ssid+kCharsPerRow);
    }
}
void draw_ip(const char* ip,bool station,bool ready,std::uint32_t state,
             const NetworkService::Transport::Status& network,std::uint8_t motion) {
    const unsigned profile=static_cast<unsigned>((state >> 8u) & 0xffu);
    // Crisp inverted status tags, IP in a full 14px font, no unnecessary chrome.
    fill_rect(0,0,31,10);
    micro_text(5,1,station ? "LAN" : "AP",false);
    micro_text(36,1,network.station_joining ? "JOIN" : ready ? "READY" : "BOOT");
    tiny_wifi(87,1,network.ready,network.station_joining,motion);
    char tag[6]{};
    std::snprintf(tag,sizeof(tag),profile<100 ? "P%02u" : "P%u",profile);
    const int tw=micro_width(tag);
    fill_rect(128-tw-4,0,tw+4,10);
    micro_text(126-tw,1,tag,false);
    line(2,12,125,12);
    // In recovery/SoftAP mode, show the network to join rather than its IP.
    // After joining the configured Wi-Fi network, show that network's real IP.
    if (!station && ready && network.ap_active) {
        const char* ssid=NetworkService::ssid();
        if (ssid && *ssid) { draw_ap_ssid(ssid); return; }
    }
    glyph16(OledGlyphs::Icon::Wifi,2,15);
    if (!ready || !station || !valid_ipv4(ip)) {
        draw_text(24,16,ready ? "NO IP" : "CONNECT",kCompact);
        // Three breathing pixels make an otherwise static wait screen feel alive.
        for (int i=0; i<3; ++i) if (static_cast<unsigned>(i)==motion%3u)
            fill_rect(104+i*7,21,3,3);
        return;
    }
    const int width=string_width(kCompact,ip);
    if (width<=102) draw_text(23+(104-width)/2,17,ip,kCompact);
    else micro_text(23+(104-micro_width(ip))/2,20,ip);
    // Accent tick shifts very slowly without disturbing IP readability.
    const int scan=static_cast<int>((motion%8u)*14u)+9;
    line(scan,31,scan+5,31);
}
void draw_profile(std::uint32_t state,std::uint8_t motion) {
    const bool usb=(state & 1u)!=0u;
    const bool owned=(state & 2u)!=0u;
    const bool fresh=(state & 4u)!=0u;
    const unsigned number=static_cast<unsigned>((state>>8u)&0xffu);
    const unsigned driver=static_cast<unsigned>((state>>16u)&0xffu);
    // Strongly different composition: inverted numeric tile + concise telemetry.
    fill_rect(0,0,43,32);
    char num[8]{};
    std::snprintf(num,sizeof(num),number<100?"%02u":"%u",number);
    const Font& num_font=number<100?kBig:kText;
    const int w=string_width(num_font,num);
    draw_text((43-w)/2,number<100?5:8,num,num_font,false);
    micro_text(49,2,"PROFILE");
    glyph16(OledGlyphs::Icon::Profile,110,0);
    line(49,11,103,11);
    const char* mode= !usb ? "USB OFF" : driver==1u ? "OGX" : driver==4u ? "XID" : "USB";
    draw_text(49,13,mode,kCompact);
    const bool pad_connected=(input_sources.load(std::memory_order_acquire)&3u)!=0u;
    micro_text(49,25,!usb?"DISCONNECTED":owned?(fresh?"INPUT LIVE":"INPUT WAIT"):
               pad_connected?"PAD CONNECTED":"STANDBY");
    if (owned && fresh) sparkle(98,4,motion);
}
const char* input_name(InputKind kind) {
    switch (kind) {
        case InputKind::Xbox: return "XBOX ONE/SERIES";
        case InputKind::Xbox360: return "XBOX 360";
        case InputKind::Ps3: return "DUALSHOCK 3";
        case InputKind::Ps4: return "DUALSHOCK 4";
        case InputKind::Ps5: return "DUALSENSE";
        case InputKind::SwitchPro: return "SWITCH PRO";
        case InputKind::Wii: return "WII / WII U";
        case InputKind::EightBitDo: return "8BITDO";
        case InputKind::Steam: return "STEAM PAD";
        case InputKind::GameSir: return "GAMESIR";
        default: return "GAMEPAD";
    }
}
void draw_input_page(bool wired, std::uint32_t inputs, std::uint32_t state,
                     std::uint8_t motion) {
    const auto kind = static_cast<InputKind>((inputs >> (wired ? 16u : 8u)) & 0xffu);
    const char* label = input_name(kind);
    fill_rect(0, 0, wired ? 27 : 21, 10);
    micro_text(4, 1, wired ? "USB" : "BT", false);
    micro_text(wired ? 32 : 26, 1, "CONNECTED");
    const unsigned profile = static_cast<unsigned>((state >> 8u) & 0xffu);
    char tag[7]{};
    std::snprintf(tag, sizeof(tag), "P%u", profile);
    micro_text(128 - micro_width(tag) - 2, 1, tag);
    line(2, 11, 125, 11);
    glyph16(wired?OledGlyphs::Icon::Usb:OledGlyphs::Icon::Bluetooth,2,14);
    if (string_width(kCompact, label) <= 102)
        draw_text(23+(104 - string_width(kCompact, label)) / 2, 12, label, kCompact);
    else micro_text(23+(104 - micro_width(label)) / 2, 17, label);
    // All seven footer rows fit the 32px panel, clear of the controller name.
    micro_text(24, 25, wired ? "WIRED HOST" : (inputs & 4u) ? "BLUETOOTH LE" : "CLASSIC BT");
    if ((motion & 1u) == 0u) fill_rect(123, 28, 3, 3);
}
void draw_network(const NetworkService::Transport::Status& network,std::uint8_t motion) {
    const bool station = network.station_connected && network.station_ip && *network.station_ip;
    const bool waiting = network.station_configured && !station;
    glyph16(OledGlyphs::Icon::Wifi,9,8);
    line(35,2,35,29);
    draw_text(42,0,station || waiting ? "LAN" : "AP",kText);
    // Bold white bottom ribbon is deliberately asymmetric to the home/profile.
    fill_rect(41,21,87,11);
    const char* label = station ? "CONNECTED" : waiting ?
        (network.station_joining ? "JOINING" : "RECONNECT") : network.ready ? "AP ACTIVE" : "STARTING";
    const int width=micro_width(label);
    micro_text(43+(83-width)/2,23,label,false);
    if (!station && waiting) sparkle(121,6,motion);
}
void activity_labels(Activity event,const char*& title,const char*& desc,OledGlyphs::Icon& icon) {
    title="SAVING"; desc="PROFILE"; icon=OledGlyphs::Icon::Save;
    switch (event) {
        case Activity::ProfileLoad: title="LOADING"; desc="PROFILE"; icon=OledGlyphs::Icon::Load; break;
        case Activity::NetworkLoad: title="LOADING"; desc="NETWORK"; icon=OledGlyphs::Icon::Load; break;
        case Activity::LedLoad: title="LOADING"; desc="LED"; icon=OledGlyphs::Icon::Load; break;
        case Activity::ProfileApply: title="APPLYING"; desc="PROFILE"; icon=OledGlyphs::Icon::Settings; break;
        case Activity::ProfileSave: title="SAVING"; desc="PROFILE"; icon=OledGlyphs::Icon::Save; break;
        case Activity::NetworkSave: title="SAVING"; desc="NETWORK"; icon=OledGlyphs::Icon::Save; break;
        case Activity::LedApply: title="APPLYING"; desc="LED"; icon=OledGlyphs::Icon::Settings; break;
        case Activity::LedSave: title="SAVING"; desc="LED"; icon=OledGlyphs::Icon::Save; break;
        case Activity::OledApply: title="DISPLAY"; desc="ADJUSTING"; icon=OledGlyphs::Icon::Settings; break;
        case Activity::OledSave: title="SAVING"; desc="DISPLAY"; icon=OledGlyphs::Icon::Save; break;
        case Activity::FirmwareCancel: title="UPDATE"; desc="CANCELED"; icon=OledGlyphs::Icon::Warning; break;
        case Activity::None: break;
    }
}
void draw_activity(Activity event,std::uint8_t motion) {
    const char *title, *desc;
    OledGlyphs::Icon symbol;
    activity_labels(event,title,desc,symbol);
    // Animated inverted icon tile, with progress ticks on the bottom edge.
    fill_rect(0,0,33,32);
    glyph16(symbol,8,7,false);
    micro_text(40,2,desc);
    draw_text(39,12,title,kCompact);
    // The percentage diagnostic used to draw a second title directly on top
    // of DISPLAY/ADJUSTING. The ordinary notification is sufficient now that
    // live hardware contrast control is validated. Keep the animation ticks.
    for (unsigned i=0; i<8; ++i) {
        const bool lit=i<2u || i==((motion/2u)%8u);
        if (lit) fill_rect(40+static_cast<int>(i*11u),29,6,2);
    }
    if ((motion&3u)==0u) { pixel(30,2,false); pixel(30,3,false); }
}
void draw_firmware(FirmwareUpdate::DisplayPhase phase,std::uint8_t percent,std::uint8_t motion) {
    using FirmwareUpdate::DisplayPhase;
    if (phase==DisplayPhase::Failed) {
        fill_rect(0,0,31,32);
        glyph16(OledGlyphs::Icon::Warning,7,8,false);
        micro_text(39,2,"UPDATE FAILED");
        draw_text(39,14,"ERROR",kCompact);
        return;
    }
    glyph16(OledGlyphs::Icon::Update,3,9);
    micro_text(25,2,"FIRMWARE");
    micro_text(25,16,phase==DisplayPhase::Receiving?"RECEIVE":
                         phase==DisplayPhase::Installing?"INSTALL":"READY");
    const unsigned shown=(phase==DisplayPhase::Receiving)?percent:100u;
    char number[6]{};
    std::snprintf(number,sizeof(number),"%u",shown>100?100:shown);
    const int width=string_width(kCompact,number);
    draw_text(115-width,12,number,kCompact);
    micro_text(117,16,"%");
    // Wide capsule progress bar; low-cost 2px stripes can animate during RX.
    line(3,28,124,28); line(3,31,124,31);
    const int pixels=static_cast<int>((shown>100?100:shown)*118u/100u);
    fill_rect(5,29,pixels,2);
    if (phase==DisplayPhase::Receiving && shown<100u)
        fill_rect(5+pixels,29,static_cast<int>((motion%3u)+1u),2);
}
void blend_transition() {
    if (!wiping || wipe_step>=kWipeSteps) return;
    for (unsigned ypage=0; ypage<kPages; ++ypage) {
        for (unsigned x=0; x<kWidth; ++x) {
            bool show_new=false;
            switch (wipe_style) {
                case 0: show_new=x<(kWidth*wipe_step/kWipeSteps); break; // left scan
                case 1: show_new=x>=kWidth-(kWidth*wipe_step/kWipeSteps); break; // right scan
                default: show_new=((x/8u+ypage*3u)%kWipeSteps)<wipe_step; break; // tile reveal
            }
            if (!show_new) fb[ypage*kWidth+x]=wipe_source[ypage*kWidth+x];
        }
    }
}
void draw(const NetworkService::Transport::Status& network,std::uint32_t state,
          std::uint8_t overlay,Activity event,FirmwareUpdate::DisplayPhase firmware_phase,
          std::uint8_t percent,std::uint8_t motion,std::uint32_t now) {
    std::memset(fb,0,sizeof(fb));
    if (overlay==2) draw_firmware(firmware_phase,percent,motion);
    else if (overlay==1) draw_activity(event,motion);
    else if (page==0) draw_splash(now);
    else if (page==1) {
        const bool station=network.station_connected && network.station_ip && *network.station_ip;
        draw_ip(station?network.station_ip:network.ap_ip,station,network.ready,state,network,motion);
    } else if (page==2) draw_profile(state,motion);
    else if (page==3) draw_network(network,motion);
    else draw_input_page(page==5, input_sources.load(std::memory_order_acquire), state, motion);
    blend_transition();
}
void offline(std::uint32_t now) {
    auto* hw=i2c_get_hw(i2c1);
    hw->enable=0;
    (void)hw->clr_tx_abrt;
    (void)hw->clr_stop_det;
    tx.kind=TxKind::None;
    bus_initialized=false; // Reset the controller/FIFOs at the next scheduled probe.
    online.store(false,std::memory_order_release);
    sent=0;
    stream_setup=transfer_pending=false;
    next_probe=now+kProbeMs;
}
void init_bus() {
    if (bus_initialized) return;
    i2c_init(i2c1,400000);
    gpio_set_function(DC_OGX_OLED_SDA,GPIO_FUNC_I2C);
    gpio_set_function(DC_OGX_OLED_SCL,GPIO_FUNC_I2C);
    gpio_pull_up(DC_OGX_OLED_SDA);
    gpio_pull_up(DC_OGX_OLED_SCL);
    bus_initialized=true;
}
void initialize() {
    init_bus();
    const auto bits=OledSettings::packed();
    const auto settings=decode_settings(bits);
    const std::uint8_t first=static_cast<std::uint8_t>(settings.fade && settings.enabled && settings.brightness ? 8 : settings.brightness);
    const std::uint8_t cmds[]={
        0xae,0xd5,0x80,0xa8,0x1f,0xd3,0x00,0x40,0x8d,0x14,
        0x20,0x00,static_cast<std::uint8_t>(settings.flip?0xa0:0xa1),
        static_cast<std::uint8_t>(settings.flip?0xc0:0xc8),0xda,0x02,0x81,first,
        0xd9,0xf1,0xdb,0x40,0xa4,static_cast<std::uint8_t>(settings.invert?0xa7:0xa6),
        0x2e,static_cast<std::uint8_t>(settings.enabled && settings.brightness ? 0xaf : 0xae)
    };
    command(TxKind::Initialize,cmds,sizeof(cmds),first,bits);
}
void finish_initialize(const Transfer& completed) {
    const auto settings=decode_settings(completed.settings);
    const auto first=completed.value;
    online.store(true,std::memory_order_release);
    sent=0;
    stream_setup=transfer_pending=false;
    frame_dirty=true;
    next_chunk=0;
    page=0;
    page_deadline=0;
    splash_since=0;
    previous_state=~0u;
    previous_inputs=~0u;
    previous_network_flags=0xff;
    previous_ip[0]='\0';
    displayed_overlay=0;
    displayed_event=0;
    displayed_phase=FirmwareUpdate::DisplayPhase::Idle;
    displayed_percent=255;
    animation_frame=0;
    visible_scene=0;
    wiping=false;
    wipe_step=0;
    next_motion=0;
    active_contrast=first;
    panel_on=settings.enabled && settings.brightness!=0;
    last_external_activity=0;
    previous_settings=completed.settings;
    next_contrast_step=0;
    contrast_resend_pending=false;
    brightness_apply_pending=false;
}
void finish_transfer(const Transfer& completed,std::uint32_t now) {
    switch (completed.kind) {
        case TxKind::Initialize: finish_initialize(completed); break;
        case TxKind::Settings: previous_settings=completed.settings; break;
        case TxKind::Contrast:
            active_contrast=completed.value;
            contrast_resend_pending=false;
            brightness_apply_pending=false;
            next_contrast_step=now+45u;
            break;
        case TxKind::Power:
            panel_on=completed.value!=0;
            if (!panel_on) active_contrast=0;
            sent=0;
            stream_setup=transfer_pending=false;
            frame_dirty=true;
            if (panel_on) { page=1; page_deadline=now+kIpMs; }
            break;
        case TxKind::Address: stream_setup=true; sent=0; break;
        case TxKind::Data:
            sent+=completed.size-1u;
            if (sent==sizeof(fb)) {
                sent=0;
                stream_setup=false;
                transfer_pending=false;
            }
            break;
        case TxKind::None: break;
    }
}
#endif // DC_OGX_OLED_ENABLED
} // namespace

void publish_controller(bool usb,bool owned,bool fresh,std::uint8_t profile,std::uint8_t mode) {
    shared.store((usb?1u:0u)|(owned?2u:0u)|(fresh?4u:0u)|
                 (static_cast<std::uint32_t>(profile)<<8u)|
                 (static_cast<std::uint32_t>(mode)<<16u),std::memory_order_release);
}
void publish_input_devices(bool bluetooth_connected, InputKind bluetooth_type, bool bluetooth_le,
                           bool wired_connected, InputKind wired_type) {
    const auto status = (bluetooth_connected ? 1u : 0u) |
                        (wired_connected ? 2u : 0u) |
                        (bluetooth_le ? 4u : 0u) |
                        (static_cast<std::uint32_t>(bluetooth_type) << 8u) |
                        (static_cast<std::uint32_t>(wired_type) << 16u);
    input_sources.store(status, std::memory_order_release);
}
void notify(Activity event) {
    if (event==Activity::None) return;
    const std::uint32_t serial=activity_seq.fetch_add(1u,std::memory_order_relaxed)+1u;
    activity_posted.store((serial<<8u)|static_cast<std::uint32_t>(event),std::memory_order_release);
}
bool detected() { return online.load(std::memory_order_acquire); }
void poll_transfer() {
#if DC_OGX_OLED_ENABLED
    if (tx.kind==TxKind::None) return;
    const auto result=advance_transfer();
    if (result==TxResult::Pending) return;
    const auto now=to_ms_since_boot(get_absolute_time());
    if (result==TxResult::Failed) { offline(now); return; }
    const auto completed=tx;
    tx.kind=TxKind::None;
    finish_transfer(completed,now);
#endif
}
void service(std::uint32_t now,const NetworkService::Transport::Status& network) {
#if DC_OGX_OLED_ENABLED
    poll_transfer();
    if (tx.kind!=TxKind::None) return;
    const auto posted=activity_posted.load(std::memory_order_acquire);
    if (posted!=observed_activity) {
        observed_activity=posted;
        const Activity incoming=static_cast<Activity>(posted&0xffu);
        const bool read_only=incoming==Activity::ProfileLoad || incoming==Activity::NetworkLoad ||
                             incoming==Activity::LedLoad;
        const bool busy_action=activity!=Activity::None &&
            activity!=Activity::ProfileLoad && activity!=Activity::NetworkLoad &&
            activity!=Activity::LedLoad && !due(now,activity_deadline);
        if (!(read_only && busy_action)) {
            // A repeated Apply must retry the physical 0x81 command even if
            // the JSON settings value has not changed since the last attempt.
            if (incoming==Activity::OledApply) contrast_resend_pending=true;
            activity=incoming;
            active_activity_serial=posted;
            activity_deadline=now+2500u;
            last_external_activity=now;
        }
    }
    if (!online.load(std::memory_order_acquire)) {
        if (due(now,next_probe)) initialize();
        return;
    }
    const auto firmware=FirmwareUpdate::display_progress();
    if (firmware.phase!=update_phase) {
        update_phase=firmware.phase;
        update_phase_since=now;
    }
    using FirmwareUpdate::DisplayPhase;
    const bool firmware_visible=firmware.phase==DisplayPhase::Receiving ||
        firmware.phase==DisplayPhase::Installing ||
        ((firmware.phase==DisplayPhase::Ready || firmware.phase==DisplayPhase::Failed) &&
         !due(now,update_phase_since+5000u));
    const std::uint8_t rounded_percent=static_cast<std::uint8_t>((firmware.percent/5u)*5u);
    const bool event_visible=activity!=Activity::None && !due(now,activity_deadline);
    const std::uint8_t overlay=firmware_visible?2u:event_visible?1u:0u;
    if (overlay==2) last_external_activity=now;
    if (overlay!=displayed_overlay ||
        (overlay==1 && active_activity_serial!=displayed_event) ||
        (overlay==2 && (firmware.phase!=displayed_phase || rounded_percent!=displayed_percent))) {
        if (displayed_overlay && !overlay) {
            page=1;
            page_deadline=now+kIpMs;
        }
        displayed_overlay=overlay;
        displayed_event=active_activity_serial;
        displayed_phase=firmware.phase;
        displayed_percent=rounded_percent;
        frame_dirty=true;
    }
    if (!overlay) {
        if (page_deadline==0) {
            splash_since=now;
            page_deadline=now+kSplashMs;
        } else if (due(now,page_deadline)) {
            const auto available=input_sources.load(std::memory_order_acquire);
            for (unsigned tries=0; tries<5; ++tries) {
                page=page>=5?1:static_cast<std::uint8_t>(page+1u);
                if (page==4 && !(available&1u)) continue;
                if (page==5 && !(available&2u)) continue;
                break;
            }
            page_deadline=now+(page==1?kIpMs:page==2?kProfileMs:
                               page==3?kNetworkMs:kInputMs);
            frame_dirty=true;
        }
    }
    const std::uint32_t state=shared.load(std::memory_order_acquire);
    const std::uint32_t sources=input_sources.load(std::memory_order_acquire);
    if (sources!=previous_inputs) {
        const std::uint32_t previous=previous_inputs;
        previous_inputs=sources;
        last_external_activity=now;
        frame_dirty=true;
        if (!overlay) {
            if ((sources&2u) && !(previous&2u)) page=5;
            else if ((sources&1u) && !(previous&1u)) page=4;
            else if ((page==4 && !(sources&1u)) || (page==5 && !(sources&2u))) page=1;
            if (page==4 || page==5) page_deadline=now+kInputMs;
            else if (page==1) page_deadline=now+kIpMs;
        }
    }
    const bool lan=network.station_connected && network.station_ip && *network.station_ip;
    const char* ip=lan?network.station_ip:network.ap_ip;
    const std::uint8_t flags=static_cast<std::uint8_t>(
        (network.ready?1u:0u)|(lan?2u:0u)|
        (network.station_joining?4u:0u)|(network.station_configured?8u:0u));
    if (state!=previous_state || flags!=previous_network_flags ||
        std::strncmp(previous_ip,ip?ip:"",sizeof(previous_ip)-1)!=0) {
        previous_state=state;
        previous_network_flags=flags;
        std::snprintf(previous_ip,sizeof(previous_ip),"%s",ip?ip:"");
        last_external_activity=now;
        frame_dirty=true;
    }
    // Apply configuration changes only at I2C frame boundaries. This avoids
    // corrupting the GDDRAM column pointer on controller-compatible clones.
    const auto bits=OledSettings::packed();
    const auto settings=decode_settings(bits);
    if (bits!=previous_settings && !transfer_pending) {
        const auto old=previous_settings;
        last_external_activity=now;
        brightness_apply_pending|=((old^bits)&0x0000ffffu)!=0u ||
                                  ((old^bits)&(1u<<24))!=0u;
        frame_dirty=true;
        if ((old^bits)&((1u<<25)|(1u<<26))) {
            const std::uint8_t cmds[]={
                static_cast<std::uint8_t>(settings.flip?0xa0:0xa1),
                static_cast<std::uint8_t>(settings.flip?0xc0:0xc8),
                static_cast<std::uint8_t>(settings.invert?0xa7:0xa6)};
            command(TxKind::Settings,cmds,sizeof(cmds),0,bits);
            return;
        }
        previous_settings=bits; // No orientation command is needed.
    }
    const bool timed_off=!overlay && settings.off_index &&
        due(now,last_external_activity+
            static_cast<std::uint32_t>(OledSettings::TIMEOUTS_S[settings.off_index])*1000u);
    const bool should_sleep=!settings.enabled || timed_off;
    const bool should_dim=!overlay && settings.dim_index &&
        due(now,last_external_activity+
            static_cast<std::uint32_t>(OledSettings::TIMEOUTS_S[settings.dim_index])*1000u);
    const std::uint8_t target_contrast=should_dim?settings.idle_brightness:settings.brightness;
    // At 0%, send the controller's *display-off* instruction. Contrast 0x00
    // alone can still leave pixels glowing on SSD1306-compatible panels.
    const bool want_panel_on=!should_sleep && target_contrast!=0;
    if (!transfer_pending && panel_on!=want_panel_on) {
        if ((want_panel_on && active_contrast!=target_contrast) ||
            (!want_panel_on && target_contrast==0 && active_contrast!=0) ||
            contrast_resend_pending) {
            send_contrast(want_panel_on?target_contrast:0);
            return;
        }
        const std::uint8_t power[]={static_cast<std::uint8_t>(want_panel_on?0xaf:0xae)};
        command(TxKind::Power,power,sizeof(power),want_panel_on?1:0);
        return;
    }
    if (!panel_on) return;
    // Change contrast at framebuffer boundaries, and commit it only after STOP.
    if (!transfer_pending && (active_contrast!=target_contrast || contrast_resend_pending) &&
        (brightness_apply_pending || contrast_resend_pending || due(now,next_contrast_step))) {
        constexpr unsigned kStep=24;
        const unsigned delta=active_contrast<target_contrast ?
            static_cast<unsigned>(target_contrast-active_contrast) :
            static_cast<unsigned>(active_contrast-target_contrast);
        const unsigned step=(brightness_apply_pending || contrast_resend_pending)?delta:(delta<kStep?delta:kStep);
        const auto next=static_cast<std::uint8_t>(active_contrast<target_contrast?
            active_contrast+step:active_contrast-step);
        send_contrast(next);
        return;
    }
    // Select the scene after connection events have selected their page, and
    // only after the prior framebuffer is fully sent. Connection/update pages
    // cut directly so a transition cannot hide a disconnect or urgent update.
    const std::uint8_t scene=overlay==2 ? 7u : overlay==1 ? 6u : page;
    if (!transfer_pending && scene!=visible_scene) {
        wiping=scene!=4 && scene!=5 && scene!=7 &&
               visible_scene!=4 && visible_scene!=5 && visible_scene!=7;
        if (wiping) std::memcpy(wipe_source,fb,sizeof(fb));
        visible_scene=scene;
        wipe_style=scene==2?2u:scene==3?0u:scene==6?0u:2u;
        wipe_step=1;
        next_wipe_step=now+kWipeStepMs;
        frame_dirty=true;
    }
    // Don't advance the animation until all 512 bytes of the prior frame have
    // been transmitted. Every frame remains coherent even on a slow service loop.
    if (!transfer_pending && wiping && due(now,next_wipe_step)) {
        if (++wipe_step>=kWipeSteps) wiping=false;
        next_wipe_step=now+kWipeStepMs;
        frame_dirty=true;
    }
    if (!transfer_pending && !wiping && due(now,next_motion)) {
        const std::uint32_t period=page==0 && overlay==0 ? 170u :
            overlay==1 ? 240u : overlay==2 ? 240u :
            page==3 ? 320u : page==2 ? 840u : 1220u;
        next_motion=now+period;
        ++animation_frame;
        frame_dirty=true;
    }
    if (sent==0 && !stream_setup && frame_dirty) {
        draw(network,state,overlay,activity,firmware.phase,rounded_percent,animation_frame,now);
        frame_dirty=false;
        transfer_pending=true;
        next_chunk=now;
    }
    if (!due(now,next_chunk) || !transfer_pending) return;
    next_chunk=now+2u;
    if (!stream_setup) {
        const std::uint8_t addr[]={0x21,0,127,0x22,0,3};
        command(TxKind::Address,addr,sizeof(addr));
        return;
    }
    std::uint8_t packet[kBurst+1];
    packet[0]=0x40;
    const auto count=sizeof(fb)-sent<kBurst?sizeof(fb)-sent:kBurst;
    std::memcpy(packet+1,fb+sent,count);
    start_transfer(TxKind::Data,packet,count+1);
#else
    (void)now; (void)network;
#endif
}
} // namespace OledDisplay

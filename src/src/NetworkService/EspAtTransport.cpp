#include "NetworkService/Transport.h"
#include "NetworkService/EspAtProtocol.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "Board/Config.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/regs/uart.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/time.h"

namespace NetworkService::Transport {
namespace {
constexpr std::uint32_t RX_CAPACITY = 8192;
constexpr std::uint32_t RX_MASK = RX_CAPACITY - 1;
constexpr unsigned RX_POLL_BUDGET = 2048;
static_assert((RX_CAPACITY & RX_MASK) == 0, "RX ring must be a power of two");
static_assert(OGXM_ESP_UART_ID == 0 || OGXM_ESP_UART_ID == 1, "Invalid ESP UART");

EspAt::Engine engine;
std::uint8_t rx_bytes[RX_CAPACITY];
std::atomic<std::uint32_t> rx_head{0};
std::atomic<std::uint32_t> rx_tail{0};
std::atomic<bool> rx_error{false};
bool started = false;

uart_inst_t* const esp_uart = OGXM_ESP_UART_ID == 0 ? uart0 : uart1;
constexpr unsigned ESP_UART_IRQ = OGXM_ESP_UART_ID == 0 ? UART0_IRQ : UART1_IRQ;

void uart_irq() {
    auto head = rx_head.load(std::memory_order_relaxed);
    while (uart_is_readable(esp_uart)) {
        const std::uint32_t value = uart_get_hw(esp_uart)->dr;
        // PL011 receive status: framing, parity, break and FIFO overrun.
        if (value & 0xf00u) rx_error.store(true, std::memory_order_relaxed);
        const auto next = (head + 1) & RX_MASK;
        if (next == rx_tail.load(std::memory_order_acquire)) {
            rx_error.store(true, std::memory_order_relaxed);
        } else {
            rx_bytes[head] = static_cast<std::uint8_t>(value);
            head = next;
            rx_head.store(head, std::memory_order_release);
        }
    }
    // Clear sticky receive errors after recording them for protocol recovery.
    uart_get_hw(esp_uart)->rsr = 0;
}

std::size_t uart_write(void*, const std::uint8_t* bytes, std::size_t length) {
    std::size_t count = 0;
    while (count != length && uart_is_writable(esp_uart)) {
        uart_get_hw(esp_uart)->dr = bytes[count++];
    }
    return count;
}

bool uart_idle(void*) {
    return (uart_get_hw(esp_uart)->fr & UART_UARTFR_BUSY_BITS) == 0;
}

void discard_rx(void*) {
    const auto interrupts = save_and_disable_interrupts();
    while (uart_is_readable(esp_uart)) (void)uart_get_hw(esp_uart)->dr;
    rx_tail.store(rx_head.load(std::memory_order_relaxed), std::memory_order_relaxed);
    rx_error.store(false, std::memory_order_relaxed);
    uart_get_hw(esp_uart)->rsr = 0;
    restore_interrupts(interrupts);
}

void set_baud(void*, std::uint32_t baud) {
    // Engine waits until its command is drained before changing baud. No
    // persistent UART_DEF command and no unverified reset/enable GPIO is used.
    uart_set_baudrate(esp_uart, baud);
}
} // namespace

bool start(const Config& config, const Callbacks& callbacks) {
    if (started) return true;
    uart_init(esp_uart, OGXM_ESP_UART_BAUD);
    uart_set_format(esp_uart, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(esp_uart, false, false);
    uart_set_fifo_enabled(esp_uart, true);
    gpio_set_function(OGXM_ESP_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(OGXM_ESP_UART_RX_PIN, GPIO_FUNC_UART);
    irq_set_exclusive_handler(ESP_UART_IRQ, uart_irq);
    // A 32-byte FIFO fills in <0.7ms at 460800. Prioritize RX over default
    // background timers; the 8KiB ring absorbs bounded work in the core1 loop.
    irq_set_priority(ESP_UART_IRQ, 0x40);
    irq_set_enabled(ESP_UART_IRQ, true);
    uart_set_irq_enables(esp_uart, true, false);
    started = engine.begin(config, callbacks,
        {nullptr, uart_write, uart_idle, set_baud, discard_rx}, OGXM_ESP_UART_BAUD,
        to_ms_since_boot(get_absolute_time()));
    if (!started) {
        uart_set_irq_enables(esp_uart, false, false);
        irq_set_enabled(ESP_UART_IRQ, false);
        uart_deinit(esp_uart);
    }
    return started;
}

void poll(std::uint32_t now_ms) {
    if (!started) return;
    if (rx_error.exchange(false, std::memory_order_acq_rel)) {
        discard_rx(nullptr);
        engine.receive_overrun();
    }
    unsigned budget = RX_POLL_BUDGET;
    std::uint8_t chunk[256];
    while (budget) {
        const auto tail = rx_tail.load(std::memory_order_relaxed);
        const auto head = rx_head.load(std::memory_order_acquire);
        if (tail == head) break;
        const auto available = (head - tail) & RX_MASK;
        const auto contiguous = RX_CAPACITY - tail;
        const auto count = std::min<std::uint32_t>({available, contiguous, budget, 256u});
        // A baud-change response can discard the remaining ring from inside
        // feed(). Consume a stable local copy before entering those callbacks.
        std::memcpy(chunk, rx_bytes + tail, count);
        rx_tail.store((tail + count) & RX_MASK, std::memory_order_release);
        if (rx_error.exchange(false, std::memory_order_acq_rel)) {
            discard_rx(nullptr);
            engine.receive_overrun();
            break;
        }
        engine.feed(chunk, count);
        budget -= count;
    }
    engine.poll(now_ms);
}

void set_control_active(bool active) { engine.set_control_active(active); }
bool retry_connection() { return engine.retry_connection(); }
bool retry_mdns() { return engine.retry_mdns(); }
bool request_scan() { return engine.request_scan(); }
const WifiScan::Status& scan_status() { return engine.scan_status(); }

std::size_t write(Handle handle, const std::uint8_t* bytes, std::size_t length) {
    return engine.write(handle, bytes, length);
}
void flush(Handle) {}
void close(Handle handle, bool abort) { engine.close(handle, abort); }
Status status() { return engine.status(); }
void start_radio_poll() {}

} // namespace NetworkService::Transport

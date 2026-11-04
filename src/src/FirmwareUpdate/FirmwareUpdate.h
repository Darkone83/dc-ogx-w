#pragma once

#include <cstddef>
#include <cstdint>

namespace FirmwareUpdate {
// Read-only atomic summary for OLED rendering; no upload mutex or flash reads.
enum class DisplayPhase : std::uint8_t { Idle = 0, Receiving, Ready, Installing, Failed };
struct DisplayProgress { DisplayPhase phase; std::uint8_t percent; };
DisplayProgress display_progress();

struct HttpResult {
    std::uint16_t status;
    std::size_t length;
    // The server must call commit_after_response only after the entire response
    // completes. It must cancel_install if that response is abandoned.
    bool install_response;
};

// Network context: bounded mailbox only; never writes flash or blocks on Core0.
HttpResult handle_request(const char* method, const char* path,
                          const char* json, std::size_t length,
                          char* output, std::size_t capacity, std::uint32_t now_ms);
// Core0 main loop. Each visit executes at most one pending upload command.
// Flash writes pause the other core safely; both cores must have called
// flash_safe_execute_core_init before any upload is possible.
void tick(std::uint32_t now_ms);
// Available on both cores; remote-control acquisition and physical output must
// be inhibited while true. The main loop must send neutral before the first
// tick that handles an update job (minimum 20 ms is enforced internally).
bool active();
bool failed();
void commit_after_response(); // Core0, after full install response + settling.
void cancel_install();       // Network side; pending install response failed.

} // namespace FirmwareUpdate

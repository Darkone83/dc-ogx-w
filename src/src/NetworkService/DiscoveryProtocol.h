#pragma once
#include <cstddef>
#include <cstdint>

// Device discovery wire format (version 1). The later Windows controller
// application will use UDP unicast with a separate session protocol.
namespace DiscoveryProtocol {
constexpr std::uint16_t kPort = 38473;
constexpr std::uint32_t kBeaconIntervalMs = 5000;
constexpr char kProbe[] = "DC-OGX-W|DISCOVER|1";

// Exact-match query prevents arbitrary UDP input from triggering broadcasts.
bool is_probe(const std::uint8_t* payload, std::size_t length);

// ASCII single datagram, no credentials or network configuration secrets.
// Returns 0 on invalid or truncated output.
std::size_t make_announcement(char* buffer, std::size_t capacity,
                              const char* device_id, const char* board,
                              const char* address, std::uint16_t http_port = 80);
} // namespace DiscoveryProtocol

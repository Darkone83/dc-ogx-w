#pragma once

#include <cstddef>
#include <cstdint>

// Bounded scan data shared by the ESP AT and CYW43 backends. All access stays
// in the serialized radio context; no callback allocates or writes flash.
namespace NetworkService::WifiScan {

constexpr std::size_t MAX_RESULTS = 16;
enum class Phase : std::uint8_t { Idle, Queued, Running, Complete, Failed };
enum class Security : std::uint8_t {
    Open, Wep, Wpa, Wpa2, WpaWpa2, Wpa3, Wpa2Wpa3, Enterprise, Unknown
};

struct Network {
    char ssid[33]{};
    std::uint8_t bssid[6]{};
    std::int16_t rssi{};
    std::uint8_t channel{};
    Security security{Security::Unknown};
};

struct Status {
    Phase phase{Phase::Idle};
    std::uint32_t generation{};
    std::uint32_t completed_at_ms{};
    std::uint8_t count{};
    bool truncated{};
    const char* error = "";
    Network networks[MAX_RESULTS]{};
};

class Cache {
public:
    const Status& status() const { return status_; }
    void reset();
    // True only when a fresh generation is queued. Queued/running requests
    // coalesce and preserve their current results and generation.
    bool queue();
    void start();
    // Only complete, visible UTF-8 SSIDs and sane 2.4 GHz results are kept.
    // Duplicate SSID/security or BSSID entries use the stronger observation.
    // At capacity, weaker results are dropped and truncated is set.
    bool add(const Network& network);
    void complete(std::uint32_t now_ms);
    // Error codes are fixed strings with static lifetime, not borrowed UART data.
    void fail(const char* error, std::uint32_t now_ms);
private:
    Status status_{};
};

const char* phase_name(Phase phase);
const char* security_name(Security security);

} // namespace NetworkService::WifiScan

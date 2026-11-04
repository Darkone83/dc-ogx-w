#include "NetworkService/WifiScan.h"

#include <algorithm>
#include <cstring>

namespace NetworkService::WifiScan {
namespace {

bool visible_utf8(const char (&ssid)[33]) {
    std::size_t size = 0;
    while (size < sizeof(ssid) && ssid[size]) ++size;
    if (!size || size > 32) return false;
    // Backends zero-initialize each record. Reject an embedded NUL instead of
    // presenting a truncated SSID which would connect to a different network.
    for (std::size_t i = size + 1; i < sizeof(ssid); ++i)
        if (ssid[i]) return false;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(ssid);
    for (std::size_t i = 0; i < size;) {
        const std::uint8_t first = bytes[i++];
        unsigned remaining = 0;
        std::uint32_t point = first;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1; point = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2; point = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3; point = first & 7; minimum = 0x10000;
        } else if (first >= 0x80) return false;
        if (remaining > size - i) return false;
        while (remaining--) {
            const std::uint8_t next = bytes[i++];
            if ((next & 0xc0) != 0x80) return false;
            point = (point << 6) | (next & 0x3f);
        }
        if (point < minimum || point > 0x10ffff ||
            (point >= 0xd800 && point <= 0xdfff) || point < 0x20 ||
            (point >= 0x7f && point <= 0x9f)) return false;
    }
    return true;
}

bool has_bssid(const Network& network) {
    for (auto value : network.bssid)
        if (value) return true;
    return false;
}

bool stronger(const Network& a, const Network& b) {
    if (a.rssi != b.rssi) return a.rssi > b.rssi;
    const int name = std::strcmp(a.ssid, b.ssid);
    if (name) return name < 0;
    return static_cast<unsigned>(a.security) < static_cast<unsigned>(b.security);
}
} // namespace

void Cache::reset() { status_ = Status{}; }

bool Cache::queue() {
    if (status_.phase == Phase::Queued || status_.phase == Phase::Running) return false;
    std::uint32_t generation = status_.generation + 1;
    if (!generation) ++generation;
    reset();
    status_.generation = generation;
    status_.phase = Phase::Queued;
    return true;
}

void Cache::start() {
    if (status_.phase == Phase::Queued) status_.phase = Phase::Running;
}

bool Cache::add(const Network& network) {
    if (status_.phase != Phase::Running || !visible_utf8(network.ssid) ||
        network.rssi < -127 || network.rssi > 0 || network.channel < 1 || network.channel > 14 ||
        static_cast<unsigned>(network.security) > static_cast<unsigned>(Security::Unknown)) return false;
    Network candidate = network;
    for (unsigned i = 0; i < status_.count;) {
        const auto& existing = status_.networks[i];
        const bool same_name = existing.security == network.security && !std::strcmp(existing.ssid, network.ssid);
        const bool same_bssid = has_bssid(network) && !std::memcmp(existing.bssid, network.bssid, sizeof(network.bssid));
        if (!same_name && !same_bssid) { ++i; continue; }
        if (existing.rssi > candidate.rssi) candidate = existing;
        // One observation can match one row's name and another row's BSSID.
        // Collapse all matches instead of leaving two aliases in the picker.
        for (unsigned j = i + 1; j < status_.count; ++j)
            status_.networks[j - 1] = status_.networks[j];
        --status_.count;
    }
    if (status_.count < MAX_RESULTS) status_.networks[status_.count++] = candidate;
    else {
        status_.truncated = true;
        if (!stronger(candidate, status_.networks[MAX_RESULTS - 1])) return false;
        status_.networks[MAX_RESULTS - 1] = candidate;
    }
    std::sort(status_.networks, status_.networks + status_.count, stronger);
    return true;
}

void Cache::complete(std::uint32_t now_ms) {
    if (status_.phase != Phase::Running) return;
    status_.phase = Phase::Complete;
    status_.completed_at_ms = now_ms;
    status_.error = "";
}

void Cache::fail(const char* error, std::uint32_t now_ms) {
    if (status_.phase != Phase::Queued && status_.phase != Phase::Running) return;
    status_.phase = Phase::Failed;
    status_.completed_at_ms = now_ms;
    status_.error = error && error[0] ? error : "scan_failed";
}

const char* phase_name(Phase phase) {
    switch (phase) {
    case Phase::Queued: return "queued";
    case Phase::Running: return "running";
    case Phase::Complete: return "complete";
    case Phase::Failed: return "failed";
    default: return "idle";
    }
}

const char* security_name(Security security) {
    switch (security) {
    case Security::Open: return "open";
    case Security::Wep: return "wep";
    case Security::Wpa: return "wpa";
    case Security::Wpa2: return "wpa2";
    case Security::WpaWpa2: return "wpa_wpa2";
    case Security::Wpa3: return "wpa3";
    case Security::Wpa2Wpa3: return "wpa2_wpa3";
    case Security::Enterprise: return "enterprise";
    default: return "unknown";
    }
}

} // namespace NetworkService::WifiScan

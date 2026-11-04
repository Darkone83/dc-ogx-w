#include "DiscoveryProtocol.h"
#include <cstdio>
#include <cstring>

namespace DiscoveryProtocol {
namespace {
bool safe_token(const char* value, bool address = false) {
    if (!value || !*value) return false;
    unsigned count = 0;
    for (const auto* ch = value; *ch; ++ch) {
        const bool alnum = (*ch >= '0' && *ch <= '9') ||
                           (*ch >= 'A' && *ch <= 'Z') ||
                           (*ch >= 'a' && *ch <= 'z');
        if (!(alnum || *ch == '-' || *ch == '_' || (address && *ch == '.'))) return false;
        if (++count > 48) return false;
    }
    return true;
}
} // namespace

bool is_probe(const std::uint8_t* bytes, std::size_t length) {
    constexpr std::size_t kLength = sizeof(kProbe) - 1;
    return bytes && length == kLength && std::memcmp(bytes, kProbe, kLength) == 0;
}

std::size_t make_announcement(char* buffer, std::size_t capacity,
                              const char* device_id, const char* board,
                              const char* address, std::uint16_t http_port) {
    if (!buffer || capacity < 2 || !http_port || !safe_token(device_id) ||
        !safe_token(board) || !safe_token(address, true)) return 0;
    const int written = std::snprintf(buffer, capacity,
        "DC-OGX-W|1|id=%s|board=%s|ip=%s|http=%u\n",
        device_id, board, address, static_cast<unsigned>(http_port));
    return written > 0 && static_cast<std::size_t>(written) < capacity
        ? static_cast<std::size_t>(written) : 0;
}
} // namespace DiscoveryProtocol

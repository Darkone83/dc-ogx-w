#pragma once

#include <cstddef>
#include <cstdint>

// Small, allocation-free framing helpers. Kept independent of lwIP/Pico so the
// exact parser used on the board can be exercised by the host regression tests.
namespace WebProtocol {

constexpr size_t MAX_HEADER = 2048;
constexpr size_t MAX_BODY = 4096;
constexpr size_t MAX_WS_MESSAGE = 1024;

enum class ParseResult { Incomplete, Complete, Invalid, TooLarge };

struct Request {
    char method[8]{};
    char path[96]{};
    char host[96]{};
    char origin[128]{};
    char websocket_key[32]{};
    size_t header_length{};
    size_t body_length{};
    bool websocket{};
    bool json{};
};

ParseResult parse_request(const uint8_t* data, size_t size, Request& request);
bool same_origin(const Request& request);
bool websocket_accept(const char* key, char output[29]);

struct Frame {
    uint8_t opcode{};
    bool final{};
    size_t header_length{};
    size_t payload_length{};
    uint8_t mask[4]{};
};

ParseResult parse_frame(const uint8_t* data, size_t size, Frame& frame);
void unmask(uint8_t* payload, size_t size, const uint8_t mask[4]);
bool valid_utf8(const uint8_t* payload, size_t size);
bool valid_close(const uint8_t* payload, size_t size);
size_t frame_header(uint8_t opcode, size_t length, uint8_t* output, size_t capacity);

} // namespace WebProtocol

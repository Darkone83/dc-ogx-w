#pragma once

#include <cstddef>
#include <cstdint>

namespace FirmwareUpdate {

constexpr std::uint32_t FLASH_BASE = 0x10000000;
constexpr std::uint32_t STAGING_OFFSET = 0x00100000;
constexpr std::uint32_t MAX_IMAGE_SIZE = 0x000fa000; // 1000 KiB per image.
constexpr std::uint32_t LAYOUT_VERSION = 1;
constexpr std::size_t HEADER_SIZE = 64;
constexpr std::size_t MAX_CHUNK = 1024;
constexpr std::size_t PAGE_SIZE = 256;
constexpr std::size_t SECTOR_SIZE = 4096;
constexpr std::uint32_t SESSION_TIMEOUT_MS = 120000;

enum class Board : std::uint32_t { PicoW = 1, PicoW2023 = 2 };

struct ImageHeader {
    Board board{};
    std::uint32_t image_size{};
    std::uint32_t image_crc{};
};

// Package header is little-endian, independent of host/compiler struct layout.
// CRC32 uses the usual reflected IEEE polynomial, the same as Python zlib.
std::uint32_t crc32_update(std::uint32_t state, const std::uint8_t*, std::size_t);
std::uint32_t crc32(const std::uint8_t*, std::size_t);
std::uint32_t boot2_crc(const std::uint8_t*); // RP2040 ROM's non-reflected CRC.
const char* parse_header(const std::uint8_t*, std::size_t, Board, ImageHeader&);
const char* validate_image(const std::uint8_t*, const ImageHeader&, bool check_crc = true);

enum class State : std::uint8_t { Idle, Receiving, Ready, AwaitingInstall, Failed };
const char* state_name(State);

struct Storage {
    // write is called on Core0 only. offset is page aligned, and size is page
    // aligned except the final image chunk. Reads remain XIP-safe while idle.
    bool (*write)(std::uint32_t offset, const std::uint8_t*, std::size_t);
    const std::uint8_t* bytes;
};

// Portable upload transaction; only the owner (Core0) may mutate it. An upload
// has no persistent install marker: an interrupted transfer cannot boot itself.
class Session {
public:
    explicit Session(Storage storage) : storage_(storage) {}
    const char* begin(const std::uint8_t*, std::size_t, Board,
                      std::uint32_t token, std::uint32_t now);
    const char* chunk(std::uint32_t token, std::uint32_t offset,
                      const std::uint8_t*, std::size_t, std::uint32_t now);
    const char* finish(std::uint32_t token, std::uint32_t now);
    const char* request_install(std::uint32_t token, std::uint32_t now);
    const char* cancel(std::uint32_t token);
    void cancel_install();
    bool expire(std::uint32_t now);
    void fail(const char* error);

    State state() const { return state_; }
    bool active() const { return state_ == State::Receiving || state_ == State::Ready || state_ == State::AwaitingInstall; }
    std::uint32_t token() const { return token_; }
    std::uint32_t received() const { return received_; }
    const ImageHeader& header() const { return header_; }
    const char* error() const { return error_; }

private:
    Storage storage_;
    ImageHeader header_{};
    State state_{State::Idle};
    std::uint32_t token_{}, received_{}, last_activity_{};
    std::uint32_t running_crc_{0xffffffff};
    std::uint32_t last_offset_{}, last_length_{};
    const char* error_{""};
};

} // namespace FirmwareUpdate

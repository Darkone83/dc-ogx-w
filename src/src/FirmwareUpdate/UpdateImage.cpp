#include "FirmwareUpdate/UpdateImage.h"

#include <cstring>

namespace FirmwareUpdate {
namespace {
constexpr std::uint8_t PACKAGE_MAGIC[8] = {'D','C','O','G','X','W','1',0};
constexpr std::uint8_t IDENTITY_MAGIC[16] = {'D','C','O','G','X','W','-','I','M','A','G','E','-','V','1',0};
std::uint32_t u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
// Pico SDK 2.3 Arm-secure RP2350 images start with a vector table and
// advertise their boot format in a PICOBIN IMAGE_DEF within the first 4 KiB.
// This replaces RP2040's boot2 CRC check; the whole-image CRC still applies.
bool valid_rp2350_boot_block(const std::uint8_t* data, std::uint32_t size) {
    const std::uint32_t limit=size<4096u?size:4096u;
    for (std::uint32_t start=8; start+20u<=limit; start+=4u) {
        if (u32(data+start)!=0xffffded3u || u32(data+start+4)!=0x10210142u) continue;
        std::uint32_t item=start+4u;
        while (item+12u<=limit) {
            const auto word=u32(data+item);
            const auto tag=word&0xffu;
            const auto words=(word>>8)&((tag&0x80u)?0xffffu:0xffu);
            if (tag==0xffu) {
                if (words!=(item-start-4u)/4u || u32(data+item+8u)!=0xab123579u) break;
                const auto relative=static_cast<std::int32_t>(u32(data+item+4u));
                const auto next=static_cast<std::int64_t>(start)+relative;
                return relative==0 || (next>=0 && next+16<=size &&
                       u32(data+static_cast<std::uint32_t>(next))==0xffffded3u);
            }
            if (!words || words>(limit-item)/4u) break;
            item+=words*4u;
        }
    }
    return false;
}
}

std::uint32_t crc32_update(std::uint32_t state, const std::uint8_t* data, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        state ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            state = (state >> 1) ^ (0xedb88320u & (0u - (state & 1u)));
    }
    return state;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t length) {
    return crc32_update(0xffffffff, data, length) ^ 0xffffffff;
}

std::uint32_t boot2_crc(const std::uint8_t* data) {
    std::uint32_t state = 0xffffffff;
    for (unsigned i = 0; i < 252; ++i) {
        state ^= std::uint32_t(data[i]) << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            state = (state << 1) ^ (0x04c11db7u & (0u - (state >> 31)));
    }
    return state;
}

const char* parse_header(const std::uint8_t* data, std::size_t size, Board board, ImageHeader& result) {
    if (!data || size != HEADER_SIZE) return "invalid_header_size";
    if (std::memcmp(data, PACKAGE_MAGIC, sizeof PACKAGE_MAGIC)) return "not_dc_ogx_w_package";
    if (u32(data + 8) != 1) return "unsupported_package_version";
    if (u32(data + 12) != static_cast<std::uint32_t>(board)) return "wrong_board";
    if (u32(data + 16) != LAYOUT_VERSION) return "incompatible_flash_layout";
    const std::uint32_t length = u32(data + 20);
    if (length < 512 || length > MAX_IMAGE_SIZE) return "invalid_image_size";
    if (u32(data + 28) != FLASH_BASE) return "invalid_load_address";
    for (unsigned i = 36; i < HEADER_SIZE; ++i)
        if (data[i]) return "invalid_reserved_header";
    std::uint8_t header[HEADER_SIZE];
    std::memcpy(header, data, HEADER_SIZE);
    std::memset(header + 32, 0, 4);
    if (crc32(header, sizeof header) != u32(data + 32)) return "header_checksum_mismatch";
    result = {board, length, u32(data + 24)};
    return nullptr;
}

const char* validate_image(const std::uint8_t* data, const ImageHeader& header, bool check_crc) {
    if (!data || header.image_size < 512 || header.image_size > MAX_IMAGE_SIZE) return "invalid_image_size";
    if (check_crc && crc32(data, header.image_size) != header.image_crc) return "image_checksum_mismatch";
    const bool rp2350=header.board==Board::Pico2W;
    if (rp2350) {
        if (!valid_rp2350_boot_block(data,header.image_size)) return "invalid_rp2350_boot_block";
    } else if (boot2_crc(data) != u32(data + 252)) return "invalid_boot2_checksum";
    const std::uint32_t vectors=rp2350?0u:256u;
    const std::uint32_t stack = u32(data + vectors);
    const std::uint32_t reset = u32(data + vectors + 4u);
    if ((stack & 7u) || stack <= 0x20000000 || stack > (rp2350?0x20082000u:0x20042000u)) return "invalid_initial_stack";
    if (!(reset & 1u) || (reset & ~1u) < FLASH_BASE + vectors + 8u ||
        (reset & ~1u) >= FLASH_BASE + header.image_size) return "invalid_reset_vector";
    // Do not accept a valid generic Pico UF2 repackaged under our filename.
    // This marker is emitted by this project's linked updater, with board and
    // flash layout IDs. It is compatibility checking, not signature security.
    bool found = false;
    for (std::uint32_t offset = vectors + 8u; offset + 32 <= header.image_size; ++offset) {
        if (data[offset] != IDENTITY_MAGIC[0] ||
            std::memcmp(data + offset, IDENTITY_MAGIC, sizeof IDENTITY_MAGIC)) continue;
        if (u32(data + offset + 16) == static_cast<std::uint32_t>(header.board) &&
            u32(data + offset + 20) == LAYOUT_VERSION &&
            u32(data + offset + 24) == MAX_IMAGE_SIZE && u32(data + offset + 28) == STAGING_OFFSET) {
            found = true;
            break;
        }
    }
    return found ? nullptr : "missing_matching_firmware_identity";
}

const char* state_name(State state) {
    switch (state) {
        case State::Idle: return "idle";
        case State::Receiving: return "receiving";
        case State::Ready: return "ready";
        case State::AwaitingInstall: return "awaiting_install";
        case State::Failed: return "failed";
    }
    return "failed";
}

const char* Session::begin(const std::uint8_t* data, std::size_t size, Board board,
                         std::uint32_t token, std::uint32_t now) {
    if (active()) return "update_in_progress";
    ImageHeader header;
    const char* error = parse_header(data, size, board, header);
    if (error) return error;
    if (!token) return "invalid_token";
    header_ = header;
    token_ = token;
    state_ = State::Receiving;
    received_ = last_offset_ = last_length_ = 0;
    running_crc_ = 0xffffffff;
    last_activity_ = now;
    error_ = "";
    return nullptr;
}

const char* Session::chunk(std::uint32_t token, std::uint32_t offset,
                         const std::uint8_t* data, std::size_t length, std::uint32_t now) {
    if (!token || token != token_) return "wrong_update_token";
    if (state_ != State::Receiving) return "update_not_receiving";
    if (!data || !length || length > MAX_CHUNK || offset > header_.image_size ||
        length > header_.image_size - offset || (offset % PAGE_SIZE) ||
        ((length % PAGE_SIZE) && offset + length != header_.image_size)) return "invalid_chunk";
    if (offset == last_offset_ && length == last_length_ && received_ == offset + length &&
        !std::memcmp(storage_.bytes + offset, data, length)) {
        last_activity_ = now;
        return nullptr; // HTTP response lost: exact replay is harmless.
    }
    if (offset != received_) return "unexpected_chunk_offset";
    if (!storage_.write || !storage_.bytes || !storage_.write(offset, data, length) ||
        std::memcmp(storage_.bytes + offset, data, length)) {
        fail("staging_flash_write_failed");
        return error_;
    }
    running_crc_ = crc32_update(running_crc_, data, length);
    last_offset_ = offset;
    last_length_ = static_cast<std::uint32_t>(length);
    received_ += static_cast<std::uint32_t>(length);
    last_activity_ = now;
    return nullptr;
}

const char* Session::finish(std::uint32_t token, std::uint32_t now) {
    if (!token || token != token_) return "wrong_update_token";
    if (state_ == State::Ready) { last_activity_ = now; return nullptr; }
    if (state_ != State::Receiving) return "update_not_receiving";
    if (received_ != header_.image_size) return "upload_incomplete";
    if ((running_crc_ ^ 0xffffffff) != header_.image_crc) {
        fail("image_checksum_mismatch");
        return error_;
    }
    // Every staged chunk was read back when written. A full independent CRC is
    // checked again immediately before installation, prior to any app erases.
    const char* error = validate_image(storage_.bytes, header_, false);
    if (error) { fail(error); return error; }
    state_ = State::Ready;
    last_activity_ = now;
    error_ = "";
    return nullptr;
}

const char* Session::request_install(std::uint32_t token, std::uint32_t now) {
    if (!token || token != token_) return "wrong_update_token";
    if (state_ != State::Ready && state_ != State::AwaitingInstall) return "update_not_ready";
    state_ = State::AwaitingInstall;
    last_activity_ = now;
    return nullptr;
}

const char* Session::cancel(std::uint32_t token) {
    if (!token || token != token_) return "wrong_update_token";
    state_ = State::Idle;
    token_ = received_ = 0;
    header_ = {};
    error_ = "";
    return nullptr;
}

void Session::cancel_install() {
    if (state_ == State::AwaitingInstall) state_ = State::Ready;
}

bool Session::expire(std::uint32_t now) {
    if (!active() || static_cast<std::uint32_t>(now - last_activity_) < SESSION_TIMEOUT_MS) return false;
    fail("update_session_timed_out");
    return true;
}

void Session::fail(const char* error) {
    state_ = State::Failed;
    error_ = error;
}

} // namespace FirmwareUpdate

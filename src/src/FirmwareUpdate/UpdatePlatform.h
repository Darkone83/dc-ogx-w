#pragma once

#include "FirmwareUpdate/UpdateImage.h"

namespace FirmwareUpdate::Platform {

Board board();
const char* board_name();
std::uint32_t random_token();
Storage storage();
// Core0 only. Returns an error before touching the running image; a successful
// copy reboots and cannot return. The full staged checksum is checked first.
const char* install(const ImageHeader&);

} // namespace FirmwareUpdate::Platform

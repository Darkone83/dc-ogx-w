#ifndef OGXM_REMOTE_PROTOCOL_H
#define OGXM_REMOTE_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include "RemoteControl/Json.h"

namespace RemoteControl {

// Stale inputs go neutral quickly; a brief scheduling/network gap should not
// require the user to acquire a new ownership token.
constexpr uint32_t INPUT_TIMEOUT_MS = 250;
constexpr uint32_t OWNER_TIMEOUT_MS = 5000;

struct InputState {
    uint32_t token{};
    uint32_t seq{};
    uint16_t buttons{};
    uint8_t dpad{};
    int16_t lx{}, ly{}, rx{}, ry{}; // Wire coordinates: +Y is up.
    uint8_t lt{}, rt{};
};

enum class ControlResult : uint8_t { Ok, Busy, NotOwner, Stale, Invalid };
bool parse_input(const Json& json, InputState& state);
bool valid_input(const InputState& state);

// Access only while holding the service's short mailbox lock. No hardware or
// Gamepad dependency: this state machine is exercised by host regression tests.
class Lease {
public:
    static constexpr unsigned QUEUE_SIZE = 8;
    ControlResult acquire(uint32_t requested_token, uint32_t new_token, uint32_t now);
    ControlResult submit(const InputState& state, uint32_t now);
    ControlResult heartbeat(uint32_t token, uint32_t now);
    ControlResult release(uint32_t token);
    bool expire(uint32_t now);
    bool pop(InputState& state);
    void clear();

    uint32_t token() const { return token_; }
    bool active() const { return token_ != 0; }
    // remaining() is the output freshness deadline, owner_remaining() is the
    // independent session deadline. Heartbeats refresh only the latter.
    uint32_t remaining(uint32_t now) const;
    uint32_t owner_remaining(uint32_t now) const;
    uint32_t owner_deadline() const { return last_owner_rx_ + OWNER_TIMEOUT_MS; }
    bool input_active(uint32_t now) const { return active() && remaining(now) != 0; }
    uint32_t generation() const { return generation_; }
    uint32_t received() const { return received_; }
    uint32_t rejected() const { return rejected_; }
    uint32_t expired() const { return expired_; }
    uint32_t owner_expired() const { return owner_expired_; }
    uint32_t last_sequence() const { return last_seq_; }
    bool has_sequence() const { return has_seq_; }
    uint32_t dropped() const { return dropped_; }

private:
    uint32_t token_{};
    uint32_t last_rx_{};
    uint32_t last_owner_rx_{};
    uint32_t last_seq_{};
    uint32_t generation_{};
    uint32_t received_{}, rejected_{}, expired_{}, dropped_{}, owner_expired_{};
    bool has_seq_{};
    bool input_live_{};
    InputState queue_[QUEUE_SIZE]{};
    unsigned head_{}, count_{};
};

} // namespace RemoteControl
#endif

#include "RemoteControl/Protocol.h"

namespace RemoteControl {
namespace {
// Different cores can sample a timestamp just before acquiring the mailbox.
// A newer submit can win that lock first; "now - last_rx" then wraps to almost
// UINT32_MAX even though the sample is fresh. Signed serial-time comparison
// treats that small backwards observation as zero age, and still handles the
// normal 32-bit millisecond rollover. All deadlines are far below 2^31 ms.
uint32_t elapsed_since(uint32_t now, uint32_t then) {
    const uint32_t elapsed = now - then;
    return elapsed < 0x80000000u ? elapsed : 0;
}
uint32_t monotonic_sample(uint32_t now, uint32_t previous) {
    return now - previous < 0x80000000u ? now : previous;
}
} // namespace


bool valid_input(const InputState& state) {
    return state.token != 0 && state.buttons <= 0x0fff && state.dpad <= 0x0f &&
           (state.dpad & 3) != 3 && (state.dpad & 12) != 12;
}

bool parse_input(const Json& json, InputState& state) {
    if (!json.count() || json.at(0).type != Json::Type::Object) return false;
    const int type = json.find(0, "type");
    if (type >= 0 && (json.at(type).type != Json::Type::String || !json.equals(type, "input"))) return false;
    const char* names[] = {"token","seq","buttons","dpad","lx","ly","rx","ry","lt","rt"};
    const int64_t min[] = {1,0,0,0,-32768,-32768,-32768,-32768,0,0};
    const int64_t max[] = {UINT32_MAX,UINT32_MAX,4095,15,32767,32767,32767,32767,255,255};
    int64_t values[10];
    for (unsigned i = 0; i < 10; ++i)
        if (!json.integer(json.find(0, names[i]), min[i], max[i], values[i])) return false;
    for (unsigned i = 1; i < json.at(0).next; i = json.at(i + 1).next) {
        bool known = json.equals(i, "type");
        for (const char* name : names) known = known || json.equals(i, name);
        if (!known) return false;
    }
    state.token = static_cast<uint32_t>(values[0]);
    state.seq = static_cast<uint32_t>(values[1]);
    state.buttons = static_cast<uint16_t>(values[2]);
    state.dpad = static_cast<uint8_t>(values[3]);
    state.lx = static_cast<int16_t>(values[4]);
    state.ly = static_cast<int16_t>(values[5]);
    state.rx = static_cast<int16_t>(values[6]);
    state.ry = static_cast<int16_t>(values[7]);
    state.lt = static_cast<uint8_t>(values[8]);
    state.rt = static_cast<uint8_t>(values[9]);
    return valid_input(state);
}

ControlResult Lease::acquire(uint32_t requested_token, uint32_t new_token, uint32_t now) {
    expire(now);
    if (active()) {
        if (requested_token && requested_token == token_) return ControlResult::Ok;
        ++rejected_;
        return ControlResult::Busy;
    }
    if (!new_token) return ControlResult::Invalid;
    token_ = new_token;
    last_rx_ = last_owner_rx_ = now;
    has_seq_ = input_live_ = false;
    head_ = count_ = 0;
    ++generation_;
    return ControlResult::Ok;
}

ControlResult Lease::submit(const InputState& state, uint32_t now) {
    expire(now);
    if (!active() || state.token != token_) { ++rejected_; return ControlResult::NotOwner; }
    if (!valid_input(state)) { ++rejected_; return ControlResult::Invalid; }
    // RFC1982-style serial arithmetic: wrap is permitted, duplicates and old
    // packets cannot keep a stuck controller alive. Half-range jumps are old.
    const uint32_t difference = state.seq - last_seq_;
    if (has_seq_ && (difference == 0 || difference >= 0x80000000u)) {
        ++rejected_;
        return ControlResult::Stale;
    }
    if (count_ == QUEUE_SIZE) {
        head_ = (head_ + 1) % QUEUE_SIZE;
        --count_;
        ++dropped_;
    }
    queue_[(head_ + count_) % QUEUE_SIZE] = state;
    ++count_;
    has_seq_ = true;
    last_seq_ = state.seq;
    last_rx_ = monotonic_sample(now, last_rx_);
    last_owner_rx_ = monotonic_sample(now, last_owner_rx_);
    input_live_ = true;
    ++received_;
    return ControlResult::Ok;
}

ControlResult Lease::heartbeat(uint32_t token, uint32_t now) {
    expire(now);
    if (!active() || !token || token != token_) { ++rejected_; return ControlResult::NotOwner; }
    last_owner_rx_ = monotonic_sample(now, last_owner_rx_);
    return ControlResult::Ok;
}

void Lease::clear() {
    token_ = 0;
    has_seq_ = input_live_ = false;
    head_ = count_ = 0;
    ++generation_;
}

ControlResult Lease::release(uint32_t token) {
    if (!active() || !token || token != token_) { ++rejected_; return ControlResult::NotOwner; }
    clear();
    return ControlResult::Ok;
}

bool Lease::expire(uint32_t now) {
    if (!active()) return false;
    if (input_live_ && elapsed_since(now, last_rx_) >= INPUT_TIMEOUT_MS) {
        // Do not replay queued states after a gap. Keep sequence ordering and
        // ownership so only a fresh sample from this same client can resume.
        input_live_ = false;
        head_ = count_ = 0;
        ++generation_;
        ++expired_;
    }
    if (elapsed_since(now, last_owner_rx_) < OWNER_TIMEOUT_MS) return false;
    clear();
    ++owner_expired_;
    return true;
}

bool Lease::pop(InputState& state) {
    if (!count_) return false;
    state = queue_[head_];
    head_ = (head_ + 1) % QUEUE_SIZE;
    --count_;
    return true;
}

uint32_t Lease::remaining(uint32_t now) const {
    if (!active() || !input_live_) return 0;
    const uint32_t elapsed = elapsed_since(now, last_rx_);
    return elapsed < INPUT_TIMEOUT_MS ? INPUT_TIMEOUT_MS - elapsed : 0;
}

uint32_t Lease::owner_remaining(uint32_t now) const {
    if (!active()) return 0;
    const uint32_t elapsed = elapsed_since(now, last_owner_rx_);
    return elapsed < OWNER_TIMEOUT_MS ? OWNER_TIMEOUT_MS - elapsed : 0;
}

} // namespace RemoteControl

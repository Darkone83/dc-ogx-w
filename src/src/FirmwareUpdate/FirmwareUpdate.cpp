#include "FirmwareUpdate/FirmwareUpdate.h"
#include "FirmwareUpdate/UpdateImage.h"
#include "RemoteControl/Json.h"
#include "FirmwareUpdate/UpdatePlatform.h"

#include <atomic>
#include <cstring>

namespace FirmwareUpdate {
namespace {


Session session(Platform::storage());
enum class JobKind : std::uint8_t { None, Begin, Chunk, Finish, Install, Cancel };
struct Job {
    JobKind kind{JobKind::None};
    std::uint32_t token{}, offset{}, now{};
    std::size_t length{};
    alignas(4) std::uint8_t bytes[MAX_CHUNK]{};
};
struct Snapshot {
    State state{State::Idle};
    std::uint32_t token{}, received{}, total{};
    const char* error{""};
    bool processing{};
};
Snapshot snapshot;
Job pending_job;
std::atomic<bool> mailbox{false};
std::atomic<bool> job_pending{false};
std::atomic<bool> maintenance{false};
std::atomic<bool> failure{false};
std::atomic<std::uint32_t> display_word{0};
std::atomic<std::uint32_t> session_epoch{0};
std::atomic<bool> install_delivered{false};
std::atomic<bool> install_cancelled{false};
bool publish_pending{};
const char* operation_error{};
std::uint32_t active_since{};
std::uint32_t observed_epoch{};

bool try_lock() { return !mailbox.exchange(true, std::memory_order_acquire); }
void unlock() { mailbox.store(false, std::memory_order_release); }

void publish() {
    snapshot.state = session.state();
    snapshot.token = session.token();
    snapshot.received = session.received();
    snapshot.total = session.header().image_size;
    snapshot.processing = false;
    snapshot.error = operation_error ? operation_error : session.error();
    maintenance.store(session.active(), std::memory_order_release);
    failure.store(session.state() == State::Failed, std::memory_order_release);
    const DisplayPhase phase = snapshot.state == State::Receiving ? DisplayPhase::Receiving :
        snapshot.state == State::Ready ? DisplayPhase::Ready :
        snapshot.state == State::AwaitingInstall ? DisplayPhase::Installing :
        snapshot.state == State::Failed ? DisplayPhase::Failed : DisplayPhase::Idle;
    // Update images are capped at 1000 KiB; multiplication by 100 fits in
    // uint32_t and avoids a 64-bit divide on Core0's USB service path.
    static_assert(MAX_IMAGE_SIZE <= UINT32_MAX / 100u, "OLED percentage must fit in 32 bits");
    const std::uint32_t percentage = snapshot.total
        ? (snapshot.received * 100u) / snapshot.total : 0u;
    display_word.store((static_cast<std::uint32_t>(phase) << 8) |
        (percentage > 100u ? 100u : percentage), std::memory_order_release);
    publish_pending = false;
}

HttpResult error(char* output, std::size_t capacity, std::uint16_t code, const char* message) {
    RemoteControl::JsonWriter writer(output, capacity);
    writer.append("{\"ok\":false,\"error\":\"%s\"}", message);
    return {code, writer.ok() ? writer.length() : 0, false};
}

bool hex_string(const RemoteControl::Json& json, const char* raw, const char* field,
                std::uint8_t* output, std::size_t capacity, std::size_t& length) {
    const int index = json.find(0, field);
    if (!json.valid(index) || json.at(index).type != RemoteControl::Json::Type::String) return false;
    const auto& text = json.at(index);
    const std::size_t count = text.end - text.begin;
    if ((count & 1u) || count > capacity * 2 || !count) return false;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < count; i += 2) {
        const int hi = digit(raw[text.begin + i]);
        const int lo = digit(raw[text.begin + i + 1]);
        if (hi < 0 || lo < 0) return false;
        output[i / 2] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    length = count / 2;
    return true;
}

void install() {
    if (const char* invalid = Platform::install(session.header())) {
        session.fail(invalid);
        operation_error = invalid;
        publish_pending = true;
    }
}

} // namespace

HttpResult handle_request(const char* method, const char* path, const char* body, std::size_t length,
                          char* output, std::size_t capacity, std::uint32_t now) {
    if (!std::strcmp(method, "GET") && !std::strcmp(path, "/api/update")) {
        if (!try_lock()) return error(output, capacity, 503, "service_busy");
        const Snapshot current = snapshot;
        unlock();
        RemoteControl::JsonWriter writer(output, capacity);
        writer.append("{\"ok\":true,\"state\":\"%s\",\"processing\":%s,\"active\":%s,"
                      "\"received\":%lu,\"total\":%lu,\"max_chunk\":%u,\"max_image\":%lu,"
                      "\"board\":\"%s\",\"layout\":%lu,\"format\":\"ogxw\",\"error\":\"%s\"}",
                      state_name(current.state), current.processing ? "true" : "false",
                      active() ? "true" : "false", static_cast<unsigned long>(current.received),
                      static_cast<unsigned long>(current.total), unsigned(MAX_CHUNK),
                      static_cast<unsigned long>(MAX_IMAGE_SIZE), Platform::board_name(),
                      static_cast<unsigned long>(LAYOUT_VERSION), current.error);
        if (!writer.ok()) return error(output, capacity, 500, "response_too_large");
        return {200, writer.length(), false};
    }
    if (std::strcmp(method, "POST")) return error(output, capacity, 405, "method_not_allowed");
    RemoteControl::Json json;
    if (!json.parse(body, length) || json.at(0).type != RemoteControl::Json::Type::Object)
        return error(output, capacity, 400, "invalid_json");
    Job job;
    job.now = now;
    if (!std::strcmp(path, "/api/update/begin")) {
        job.kind = JobKind::Begin;
        if (!hex_string(json, body, "header", job.bytes, HEADER_SIZE, job.length))
            return error(output, capacity, 400, "invalid_header_hex");
        ImageHeader header;
        if (const char* invalid = parse_header(job.bytes, job.length, Platform::board(), header))
            return error(output, capacity, 400, invalid);
        do { job.token = Platform::random_token(); } while (!job.token);
    } else {
        std::int64_t token;
        if (!json.integer(json.find(0, "token"), 1, UINT32_MAX, token))
            return error(output, capacity, 400, "invalid_update_token");
        job.token = static_cast<std::uint32_t>(token);
        if (!std::strcmp(path, "/api/update/chunk")) {
            job.kind = JobKind::Chunk;
            std::int64_t offset;
            if (!json.integer(json.find(0, "offset"), 0, MAX_IMAGE_SIZE, offset) ||
                !hex_string(json, body, "data", job.bytes, MAX_CHUNK, job.length))
                return error(output, capacity, 400, "invalid_chunk");
            job.offset = static_cast<std::uint32_t>(offset);
        } else if (!std::strcmp(path, "/api/update/finish")) job.kind = JobKind::Finish;
        else if (!std::strcmp(path, "/api/update/install")) job.kind = JobKind::Install;
        else if (!std::strcmp(path, "/api/update/cancel")) job.kind = JobKind::Cancel;
        else return error(output, capacity, 404, "not_found");
    }
    if (!try_lock()) return error(output, capacity, 503, "service_busy");
    const char* rejected = nullptr;
    if (snapshot.processing || pending_job.kind != JobKind::None) rejected = "update_busy";
    else if (job.kind == JobKind::Begin && maintenance.load(std::memory_order_relaxed)) rejected = "update_in_progress";
    else if (job.kind != JobKind::Begin && job.token != snapshot.token) rejected = "wrong_update_token";
    else if (job.kind == JobKind::Install && snapshot.state != State::Ready && snapshot.state != State::AwaitingInstall)
        rejected = "update_not_ready";
    if (rejected) { unlock(); return error(output, capacity, 409, rejected); }
    if (job.kind == JobKind::Begin) {
        snapshot.token = job.token;
        snapshot.received = 0;
        snapshot.total = std::uint32_t(job.bytes[20]) | (std::uint32_t(job.bytes[21]) << 8) |
                         (std::uint32_t(job.bytes[22]) << 16) | (std::uint32_t(job.bytes[23]) << 24);
        snapshot.state = State::Receiving;
        // A cancelled session can be replaced between two Core0 visits, so
        // observing active=false is not sufficient to restart the neutral
        // grace period. Every accepted Begin publishes a distinct epoch.
        session_epoch.store(session_epoch.load(std::memory_order_relaxed) + 1,
                            std::memory_order_release);
        maintenance.store(true, std::memory_order_release);
        failure.store(false, std::memory_order_release);
        install_delivered.store(false, std::memory_order_release);
        install_cancelled.store(false, std::memory_order_release);
    }
    snapshot.processing = true;
    snapshot.error = "";
    pending_job = job;
    job_pending.store(true, std::memory_order_release);
    unlock();
    RemoteControl::JsonWriter writer(output, capacity);
    writer.append("{\"ok\":true,\"queued\":true,\"token\":%lu,\"installing\":%s}",
                  static_cast<unsigned long>(job.token), job.kind == JobKind::Install ? "true" : "false");
    if (!writer.ok()) {
        if (job.kind == JobKind::Install) cancel_install();
        return error(output, capacity, 500, "response_too_large");
    }
    return {static_cast<std::uint16_t>(job.kind == JobKind::Install ? 200 : 202),
            writer.length(), job.kind == JobKind::Install};
}

void tick(std::uint32_t now) {
    const bool is_active = active();
    // Core0 visits this service on every USB iteration. An idle updater must
    // not take the status mailbox or copy a 1 KiB empty job: that made normal
    // GET requests repeatedly collide with work that did not exist. Only the
    // network producer publishes jobs; Core0 clears this hint under the lock.
    if (!is_active && !job_pending.load(std::memory_order_acquire) && !publish_pending &&
        !install_cancelled.load(std::memory_order_acquire) &&
        !install_delivered.load(std::memory_order_acquire)) return;
    const auto epoch = session_epoch.load(std::memory_order_acquire);
    if (epoch != observed_epoch) { observed_epoch = epoch; active_since = now; }
    // Give the controller loop several visits to submit neutral before any
    // erase stalls USB IRQs. The integration must inhibit output while active.
    if (is_active && static_cast<std::uint32_t>(now - active_since) < 20) return;
    if (publish_pending) {
        if (!try_lock()) return;
        publish();
        unlock();
    }
    if (job_pending.load(std::memory_order_acquire)) {
        if (!try_lock()) return;
        if (pending_job.kind == JobKind::Begin) {
            // Begin can arrive after the optimistic active/epoch reads above.
            // Recheck while holding the producer's mailbox, before consuming
            // its job, so that race cannot bypass the USB-neutral grace.
            const auto queued_epoch = session_epoch.load(std::memory_order_relaxed);
            if (queued_epoch != observed_epoch) {
                observed_epoch = queued_epoch;
                active_since = now;
            }
            if (static_cast<std::uint32_t>(now - active_since) < 20) {
                unlock();
                return;
            }
        }
        const Job job = pending_job;
        pending_job.kind = JobKind::None;
        job_pending.store(false, std::memory_order_release);
        unlock();
        switch (job.kind) {
            case JobKind::Begin: operation_error = session.begin(job.bytes, job.length, Platform::board(), job.token, now); break;
            case JobKind::Chunk: operation_error = session.chunk(job.token, job.offset, job.bytes, job.length, now); break;
            case JobKind::Finish: operation_error = session.finish(job.token, now); break;
            case JobKind::Install: operation_error = session.request_install(job.token, now); break;
            case JobKind::Cancel: operation_error = session.cancel(job.token); break;
            case JobKind::None: break;
        }
        publish_pending = true;
    } else if (session.expire(now)) {
        operation_error = nullptr;
        publish_pending = true;
    }
    // Apply cancellation after the queued install job, including when an HTTP
    // connection closes before Core0 first observes that job. Consume the
    // signal only after mailbox acquisition can no longer defer this visit.
    if (install_cancelled.exchange(false, std::memory_order_acq_rel)) {
        install_delivered.store(false, std::memory_order_release);
        session.cancel_install();
        publish_pending = true;
    }
    if (session.state() == State::AwaitingInstall && install_delivered.exchange(false, std::memory_order_acq_rel)) install();
    if (publish_pending && try_lock()) { publish(); unlock(); }
}

DisplayProgress display_progress() {
    const std::uint32_t word = display_word.load(std::memory_order_acquire);
    return {static_cast<DisplayPhase>((word >> 8) & 0xffu),
            static_cast<std::uint8_t>(word & 0xffu)};
}

bool active() { return maintenance.load(std::memory_order_acquire); }
bool failed() { return failure.load(std::memory_order_acquire); }
void commit_after_response() { install_delivered.store(true, std::memory_order_release); }
void cancel_install() { install_cancelled.store(true, std::memory_order_release); }

} // namespace FirmwareUpdate

#include "NetworkService.h"
#include "Transport.h"
#include "WebProtocol.h"
#include "RemoteControl/RemoteControl.h"
#if defined(CONFIG_EN_BLUETOOTH)
#include "Bluepad32/Bluepad32.h"
#endif
#include "RemoteControl/Json.h"
#include "NetworkSettings.h"
#include "FirmwareUpdate/FirmwareUpdate.h"
#include "Board/StatusLed.h"
#include "Board/LedSettings.h"
#include "Display/OledDisplay.h"
#include "Display/OledSettings.h"
#if defined(CONFIG_EN_USB_HOST)
#include "Input/InputSlot.h"
#endif
#if defined(CONFIG_EN_BLUETOOTH)
#include "controller/uni_controller_type.h"
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "pico/unique_id.h"
#include "pico/time.h"
#include "ogxm_web_assets.h"

#ifndef OGXM_WIFI_PASSWORD
#define OGXM_WIFI_PASSWORD "ogxremote"
#endif
#ifndef OGXM_WIFI_SSID_PREFIX
#define OGXM_WIFI_SSID_PREFIX "dc-ogx-w"
#endif
#ifndef OGXM_WIFI_CHANNEL
#define OGXM_WIFI_CHANNEL 6
#endif

namespace NetworkService {
namespace {
constexpr size_t CLIENT_COUNT = Transport::MAX_CONNECTIONS;
constexpr size_t RX_SIZE = WebProtocol::MAX_HEADER + WebProtocol::MAX_BODY;
constexpr size_t REPLY_SIZE = 4096;
constexpr uint32_t HTTP_IDLE_MS = 10000;
constexpr uint32_t WS_IDLE_MS = 30000;
constexpr uint32_t SERVICE_RETRY_MS = 1;
constexpr uint32_t SERVICE_RETRY_LIMIT_MS = 50;
// An HTTP control acquire has no input sequence or USB state to go stale.
// Give the two-core mailbox longer to become available before sending a
// retryable 503. Keep the 50ms deadline for WebSocket input/heartbeats so
// stale samples cannot clog real-time controller traffic.
constexpr uint32_t ACQUIRE_RETRY_LIMIT_MS = 250;

struct Client {
    Transport::Handle handle{};
    bool transport_closing{};
    // Zero-initialize the entire client workspace in .bss, not .data.
    // on_connected() and reply()/websocket_send() explicitly set this true.
    bool all_peer_acknowledged{};
    bool websocket{};
    bool upgrade_pending{};
    bool closing{};
    bool close_after_send{};
    bool response_pending{};
    bool save_response{};
    bool save_completed{};
    bool retry_response{};
    bool mdns_retry_response{};
    bool scan_response{};
    bool dispatch_deferred{};
    bool pong_pending{};
    uint32_t pong_id{};
    // Core0 owns the controller RPC while this flag is set. No received
    // bytes or output buffer can be modified before the completion is consumed.
    bool core_pending{};
    bool core_websocket{};
    bool core_abandoned{};
    bool core_fragmented{};
    bool core_post{};
    uint32_t core_token{};
    uint32_t core_frame_length{};
    char core_path[80]{};
    char core_reply[384]{};
    uint32_t scan_generation{};
    SaveAction save_action{SaveAction::None};
    uint32_t last_activity{};
    uint32_t last_ack{};
    uint32_t control_token{};
    uint32_t dispatch_started{};
    uint32_t dispatch_due{};
    uint32_t rx_received_at{};
    uint32_t fragment_received_at{};
    uint32_t queued{};
    uint32_t completed{};
    size_t rx_length{};
    size_t header_length{};
    size_t body_length{};
    size_t fragment_length{};
    uint8_t fragment_opcode{};
    uint8_t control_opcode{};
    uint8_t control_length{};
    uint8_t control_payload[125]{};
    const uint8_t *response_body{};
    char response_header[384]{};
    char response[REPLY_SIZE]{};
    uint8_t rx[RX_SIZE + 1]{};
    uint8_t fragment[WebProtocol::MAX_WS_MESSAGE + 1]{};
};

Client clients[CLIENT_COUNT];
static_assert(CLIENT_COUNT == RemoteControl::CORE_REQUEST_SLOTS,
              "Each network client requires exactly one Core0 RPC slot");
unsigned client_slot(const Client& client) {
    return static_cast<unsigned>(&client - clients);
}
char device_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1]{};
char ap_ssid[33]{};
std::atomic<bool> service_ready{false};
std::atomic<uint8_t> save_completion{0}; // Bit 0: ready; bit 1: peer TCP acknowledgement.
std::atomic<uint32_t> runtime_bits{0};
std::atomic<uint8_t> health_bits{0};
// Bit layout: ready, AP active, STA configured, STA connected, joining.
// Release-store the flags last so Core0 never needs the mutable radio structs.
std::atomic<std::uint32_t> oled_network_flags{0};
std::atomic<std::uint32_t> oled_station_ipv4{0};
std::uint32_t pack_ipv4(const char* address) {
    if (!address || !*address) return 0;
    std::uint32_t packed = 0;
    const char* p = address;
    for (unsigned octet = 0; octet < 4; ++octet) {
        unsigned value = 0, digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 3) {
            value = value * 10u + static_cast<unsigned>(*p++ - '0');
            ++digits;
        }
        if (!digits || value > 255 || (octet < 3 && *p++ != '.')) return 0;
        packed = (packed << 8u) | value;
    }
    return *p == '\0' ? packed : 0;
}
void publish_oled_network(const Transport::Status& st) {
    // Parse only on IP changes, not on every 10-ms radio callback.
    static char last_station_ip[16]{};
    static std::uint32_t cached_ipv4{};
    const char* address = st.station_ip ? st.station_ip : "";
    if (std::strncmp(last_station_ip, address, sizeof(last_station_ip) - 1) != 0) {
        std::snprintf(last_station_ip, sizeof(last_station_ip), "%s", address);
        cached_ipv4 = pack_ipv4(last_station_ip);
    }
    oled_station_ipv4.store(cached_ipv4, std::memory_order_relaxed);
    oled_network_flags.store((st.ready ? 1u : 0u) | (st.ap_active ? 2u : 0u) |
        (st.station_configured ? 4u : 0u) | (st.station_connected ? 8u : 0u) |
        (st.station_joining ? 16u : 0u), std::memory_order_release);
}
std::atomic<SaveAction> pending_action{SaveAction::None};
std::atomic<uint8_t> install_cancel_state{0}; // 0 idle, 1 discard requested, 2 Core0 discarded.
uint32_t request_count{};
bool started{};
bool retry_queued{};
uint32_t retry_due{};
bool mdns_retry_queued{};
uint32_t mdns_retry_due{};
bool scan_queued{};
bool scan_dispatch_failed{};
uint32_t scan_due{};
uint32_t scan_requested_generation{};
uint32_t scan_failed_at{};

uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

void refresh_transport_guard() {
    Transport::set_control_active(RemoteControl::network_control_active(now_ms()) ||
        FirmwareUpdate::active() || pending_action.load(std::memory_order_acquire) != SaveAction::None);
}

// An acquire/update can be accepted inside ESP UART draining, before the same
// poll visits the join engine. Refresh on every exit, including early replies.
struct RefreshTransportGuard {
    ~RefreshTransportGuard() { refresh_transport_guard(); }
};

uint32_t next_scan_generation(uint32_t current) {
    ++current;
    return current ? current : 1;
}

// Include replies still awaiting completion, so a second browser cannot see a
// previous completed scan as the result of a newly accepted request. Each reply
// retains its generation: late completion cannot accidentally trigger a rescan.
uint32_t pending_scan_generation() {
    if (scan_queued) return scan_requested_generation;
    const auto generation = Transport::scan_status().generation;
    for (const auto& client : clients)
        if (client.handle && client.scan_response && client.scan_generation != generation)
            return client.scan_generation;
    return 0;
}

const char* scan_blocked_by(const Transport::Status& transport) {
    if (pending_action.load(std::memory_order_acquire) != SaveAction::None) return "save";
    if (FirmwareUpdate::active()) return "update";
    if (RemoteControl::network_control_active(now_ms())) return "control";
    if (transport.station_joining) return "joining";
    if (!transport.ready) return "radio";
    return "command_queue";
}

const char* station_waiting_for(const Transport::Status& transport) {
    if (!transport.station_configured || transport.station_connected || transport.station_joining) return "";
    if (pending_action.load(std::memory_order_acquire) != SaveAction::None) return "save";
    if (FirmwareUpdate::active()) return "update";
    if (RemoteControl::network_control_active(now_ms())) return "control";
    const auto phase = Transport::scan_status().phase;
    if (pending_scan_generation() || phase == WifiScan::Phase::Queued || phase == WifiScan::Phase::Running)
        return "scan";
    if (!transport.ready) return "radio";
    if (transport.station_error[0] && std::strcmp(transport.station_error, "station_starting") &&
        std::strcmp(transport.station_error, "station_waiting_for_address")) return "retry_backoff";
    return "command_queue";
}

bool supported_scan_security(WifiScan::Security security) {
    using Security = WifiScan::Security;
#if defined(CONFIG_OGXM_BOARD_PI_PICOW_2023)
    return security == Security::Open || security == Security::Wpa || security == Security::Wpa2 ||
           security == Security::WpaWpa2 || security == Security::Wpa2Wpa3;
#else
    // CYW43 reports coarse RSN capability; WPA2/RSN does not prove that a
    // particular AP also permits our configured WPA2-PSK/AES join mode.
    return security == Security::Open || security == Security::Wpa2 ||
           security == Security::WpaWpa2 || security == Security::Wpa2Wpa3;
#endif
}

size_t scan_json(char* output, size_t capacity) {
    const auto& scan = Transport::scan_status();
    const auto transport = Transport::status();
    const auto pending = pending_scan_generation();
    const bool failed = !pending && scan_dispatch_failed;
    const auto phase = pending ? WifiScan::Phase::Queued : failed ? WifiScan::Phase::Failed : scan.phase;
    const auto generation = pending ? pending : failed ? scan_requested_generation : scan.generation;
    const bool finished = phase == WifiScan::Phase::Complete || phase == WifiScan::Phase::Failed;
    const auto age = finished ? uint32_t(now_ms() - (failed ? scan_failed_at : scan.completed_at_ms)) : 0;
    RemoteControl::JsonWriter writer(output, capacity);
    writer.append("{\"ok\":true,\"state\":\"%s\",\"generation\":%lu,\"age_ms\":%lu,"
                  "\"error\":\"%s\",\"blocked_by\":\"%s\",\"truncated\":%s,\"networks\":[",
                  WifiScan::phase_name(phase), static_cast<unsigned long>(generation),
                  static_cast<unsigned long>(age), pending ? "" : failed ? "scan_not_ready" : scan.error,
                  phase == WifiScan::Phase::Queued ? scan_blocked_by(transport) : "",
                  !pending && !failed && scan.truncated ? "true" : "false");
    const unsigned count = pending || failed ? 0 : scan.count;
    for (unsigned i = 0; i < count; ++i) {
        const auto& network = scan.networks[i];
        writer.append("%s{\"ssid\":\"", i ? "," : "");
        // Cache admits only bounded visible UTF-8, so only quote/backslash
        // need escaping. A complete 16-result response fits below REPLY_SIZE.
        for (const char* p = network.ssid; *p; ++p) {
            if (*p == '"' || *p == '\\') writer.append("\\%c", *p);
            else writer.append("%c", *p);
        }
        writer.append("\",\"rssi\":%d,\"channel\":%u,\"security\":\"%s\",\"supported\":%s}",
                      static_cast<int>(network.rssi), static_cast<unsigned>(network.channel),
                      WifiScan::security_name(network.security),
                      supported_scan_security(network.security) ? "true" : "false");
    }
    writer.append("]}");
    return writer.ok() ? writer.length() : 0;
}

Client *find_client(Transport::Handle handle) {
    if (handle == Transport::INVALID_HANDLE)
        return nullptr;
    for (auto &client : clients) {
        if (client.handle == handle)
            return &client;
    }
    return nullptr;
}

void release_control(Client& client) {
    if (client.control_token) {
        RemoteControl::release_token(client.control_token, now_ms());
        client.control_token = 0;
        refresh_transport_guard();
    }
}

void forget(Client &client) {
    if (client.core_pending) {
        // The borrowed request/response buffers still belong to Core0. Keep
        // this slot reserved until the consumer publishes a completion.
        client.core_abandoned = true;
        client.transport_closing = true;
        RemoteControl::abandon_core_request(client_slot(client));
        return;
    }
    release_control(client);
    if (client.save_response && !client.save_completed) {
        switch (client.save_action) {
        case SaveAction::Network: NetworkSettings::cancel(); break;
        case SaveAction::Firmware: FirmwareUpdate::cancel_install(); break;
        case SaveAction::Led: LedSettings::cancel(); break;
        case SaveAction::Oled: OledSettings::cancel(); break;
        default: RemoteControl::cancel_save(); break;
        }
        pending_action.store(SaveAction::None, std::memory_order_release);
    }
    client.handle = Transport::INVALID_HANDLE;
    client.response_pending = false;
    client.save_response = false;
    client.retry_response = false;
    client.mdns_retry_response = false;
    client.scan_response = false;
    client.dispatch_deferred = false;
    client.rx_length = 0;
}

void close_client(Client &client, bool abort = false) {
    if (client.handle == Transport::INVALID_HANDLE)
        return;
    if (client.core_pending) {
        client.core_abandoned = true;
        RemoteControl::abandon_core_request(client_slot(client));
        // Do not drop ownership until completion: Core0 may be performing an
        // acquire right now. forget() releases it once the result is consumed.
    } else
    release_control(client);
    client.dispatch_deferred = false;
    client.transport_closing = true;
    // The backend retains its generation until the close finishes. closed()
    // releases the shared workspace, including when close() completes inline.
    Transport::close(client.handle, abort);
}

void abort_client(Client &client) {
    close_client(client, true);
}

void pump(Client &client) {
    if (!client.handle || client.transport_closing || !client.response_pending)
        return;
    const uint32_t total = static_cast<uint32_t>(client.header_length + client.body_length);
    while (client.queued < total) {
        const uint8_t *pointer;
        size_t remaining;
        if (client.queued < client.header_length) {
            pointer = reinterpret_cast<const uint8_t *>(client.response_header) + client.queued;
            remaining = client.header_length - client.queued;
        } else {
            const size_t offset = client.queued - client.header_length;
            pointer = client.response_body + offset;
            remaining = client.body_length - offset;
        }
        const size_t length = std::min(remaining, Transport::MAX_WRITE_SIZE);
        const size_t accepted = Transport::write(client.handle, pointer, length);
        if (!accepted)
            break;
        client.queued += static_cast<uint32_t>(accepted);
    }
    Transport::flush(client.handle);
}

const char *status_text(unsigned status) {
    switch (status) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Content Too Large";
    case 415:
        return "Unsupported Media Type";
    case 422:
        return "Unprocessable Content";
    case 423:
        return "Locked";
    case 429:
        return "Too Many Requests";
    case 503:
        return "Service Unavailable";
    default:
        return "Internal Server Error";
    }
}

void reply(Client &client, unsigned status, const uint8_t *body, size_t length,
           const char *type = "application/json", bool gzip = false, bool save = false) {
    client.dispatch_deferred = false;
    const int n = std::snprintf(
        client.response_header, sizeof(client.response_header),
        "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%s\r\n",
        status, status_text(status), type, static_cast<unsigned>(length),
        gzip ? "Content-Encoding: gzip\r\n" : "");
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(client.response_header)) {
        abort_client(client);
        return;
    }
    client.header_length = static_cast<size_t>(n);
    client.response_body = body;
    client.body_length = length;
    client.queued = client.completed = 0;
    client.all_peer_acknowledged = true;
    client.response_pending = true;
    client.close_after_send = true;
    client.save_response = save;
    pump(client);
}

void error_reply(Client &client, unsigned status, const char *error) {
    const int n = std::snprintf(client.response, sizeof(client.response),
                                "{\"ok\":false,\"error\":\"%s\"}", error);
    reply(client, status, reinterpret_cast<const uint8_t *>(client.response),
          n > 0 ? static_cast<size_t>(n) : 0);
}

void websocket_send(Client &client, uint8_t opcode, const uint8_t *payload, size_t length,
                    bool close_after = false) {
    if (!client.handle || client.transport_closing || client.response_pending ||
        length > sizeof(client.response))
        return;
    if (length)
        std::memcpy(client.response, payload, length);
    client.header_length = WebProtocol::frame_header(
        opcode, length, reinterpret_cast<uint8_t *>(client.response_header),
        sizeof(client.response_header));
    client.response_body = reinterpret_cast<const uint8_t *>(client.response);
    client.body_length = length;
    client.queued = client.completed = 0;
    client.all_peer_acknowledged = true;
    client.response_pending = true;
    client.close_after_send = close_after;
    pump(client);
}

void websocket_control(Client &client, uint8_t opcode, const uint8_t *payload, size_t length) {
    if (length > sizeof(client.control_payload) || client.closing)
        return;
    if (opcode == 8) {
        client.closing = true;
        release_control(client);
    }
    if (!client.response_pending) {
        websocket_send(client, opcode, payload, length, opcode == 8);
        return;
    }
    // Keep one bounded control response while data is awaiting transport completion. A close
    // supersedes a pending pong; repeated pings may coalesce to the latest pong.
    // Dropping every pong under TCP backpressure would break healthy clients.
    client.control_opcode = opcode;
    client.control_length = static_cast<uint8_t>(length);
    if (length)
        std::memcpy(client.control_payload, payload, length);
}

void websocket_close(Client &client, uint16_t code) {
    const uint8_t bytes[] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code)};
    websocket_control(client, 8, bytes, sizeof(bytes));
}

struct MessageHeader {
    enum class Kind : uint8_t { Control, Ping, InvalidPing };
    Kind kind{Kind::Control};
    uint32_t token{};
    uint32_t ping_id{};
};

[[gnu::noinline]] MessageHeader message_header(const uint8_t* payload, size_t length) {
    // Free this parser workspace before the complete input/heartbeat handler
    // creates its own. Stacking the two consumed another 1.5KiB on Core1.
    RemoteControl::Json json;
    MessageHeader header;
    int64_t submitted_token = 0;
    if (json.parse(reinterpret_cast<const char *>(payload), length) &&
        json.at(0).type == RemoteControl::Json::Type::Object) {
        if (json.equals(json.find(0, "type"), "ping")) {
            int64_t id = 0;
            if (json.integer(json.find(0, "id"), 0, UINT32_MAX, id)) {
                header.kind = MessageHeader::Kind::Ping;
                header.ping_id = static_cast<uint32_t>(id);
            } else header.kind = MessageHeader::Kind::InvalidPing;
            return header;
        }
        json.integer(json.find(0, "token"), 1, UINT32_MAX, submitted_token);
        header.token = static_cast<uint32_t>(submitted_token);
    }
    return header;
}

void application_pong(Client& client, uint32_t id) {
    if (client.closing || client.transport_closing || client.close_after_send) return;
    if (client.response_pending) {
        // Keep the most recent health probe under backpressure, without
        // replacing an RFC 6455 close/pong or allocating a growing queue.
        client.pong_pending = true;
        client.pong_id = id;
        return;
    }
    char response[64];
    const int length = std::snprintf(response, sizeof(response),
        "{\"ok\":true,\"type\":\"pong\",\"id\":%lu}", static_cast<unsigned long>(id));
    websocket_send(client, 1, reinterpret_cast<const uint8_t*>(response), static_cast<size_t>(length));
}

bool defer_busy(Client &client, unsigned status, const char* response, size_t length,
                uint32_t retry_limit_ms = SERVICE_RETRY_LIMIT_MS) {
    // This exact response guarantees that the handler rejected the operation
    // before accepting any side effect. Do not replay other errors or replies.
    constexpr char busy[] = "{\"ok\":false,\"error\":\"service_busy\"}";
    constexpr char led_busy[] = "{\"ok\":false,\"error\":\"led_busy\"}";
    // Both error codes mean the handler failed to acquire its mailbox BEFORE
    // modifying state. These are the only operations safe to retry internally.
    if (status != 503 ||
        !((length == sizeof(busy) - 1 && !std::memcmp(response, busy, length)) ||
          (length == sizeof(led_busy) - 1 && !std::memcmp(response, led_busy, length))))
        return false;
    const uint32_t now = now_ms();
    if (!client.dispatch_deferred && client.websocket) client.dispatch_started = now;
    if (uint32_t(now - client.dispatch_started) >= retry_limit_ms) return false;
    client.dispatch_deferred = true;
    client.dispatch_due = now + SERVICE_RETRY_MS;
    return true;
}

bool dispatch_message(Client &client, const uint8_t *payload, size_t length, uint32_t received_at) {
    RefreshTransportGuard guard;
    const uint32_t now = now_ms();
    const auto header = message_header(payload, length);
    if (header.kind != MessageHeader::Kind::Control) {
        client.dispatch_deferred = false;
        if (header.kind == MessageHeader::Kind::Ping) application_pong(client, header.ping_id);
        else {
            constexpr char invalid[] = "{\"ok\":false,\"error\":\"invalid_ping_id\"}";
            websocket_send(client, 1, reinterpret_cast<const uint8_t*>(invalid), sizeof(invalid) - 1);
        }
        // Health probes never acquire/bind a token or extend input/owner age.
        return true;
    }
    const uint32_t submitted_token = header.token;
    if (submitted_token) {
        for (const auto& other : clients) {
            if (&other != &client && other.handle && !other.transport_closing &&
                other.control_token == static_cast<uint32_t>(submitted_token)) {
                constexpr char conflict[] = "{\"ok\":false,\"error\":\"control_stream_in_use\"}";
                client.dispatch_deferred = false;
                websocket_send(client, 1, reinterpret_cast<const uint8_t *>(conflict), sizeof(conflict) - 1);
                return true;
            }
        }
    }
    // POST JSON is copied into the fixed Core0 RPC slot. In particular, the
    // WebSocket wire buffer is masked again below; Core0 must not borrow it.
    if (client.core_pending) return false;
    if (!RemoteControl::submit_core_request(client_slot(client), true, "POST", "",
            reinterpret_cast<const char*>(payload), length,
            client.core_reply, sizeof(client.core_reply), received_at)) {
        // Busy/oversize should not be spun in the UART callback. A new service
        // turn will retry only if the previous command has actually completed.
        client.dispatch_deferred = true;
        client.dispatch_due = now + SERVICE_RETRY_MS;
        return false;
    }
    client.core_pending = true;
    client.core_websocket = true;
    client.core_token = submitted_token;
    client.dispatch_deferred = false;
    return false;
}

void process_websocket(Client &client) {
    if (client.dispatch_deferred && static_cast<int32_t>(now_ms() - client.dispatch_due) < 0) return;
    size_t consumed = 0;
    while (client.handle && !client.transport_closing && !client.closing &&
           consumed < client.rx_length) {
        WebProtocol::Frame frame;
        const auto parsed =
            WebProtocol::parse_frame(client.rx + consumed, client.rx_length - consumed, frame);
        if (parsed == WebProtocol::ParseResult::Incomplete)
            break;
        if (parsed != WebProtocol::ParseResult::Complete) {
            websocket_close(client, parsed == WebProtocol::ParseResult::TooLarge ? 1009 : 1002);
            client.rx_length = 0;
            return;
        }
        uint8_t *payload = client.rx + consumed + frame.header_length;
        WebProtocol::unmask(payload, frame.payload_length, frame.mask);
        consumed += frame.header_length + frame.payload_length;
        if (frame.opcode == 8) {
            if (!WebProtocol::valid_close(payload, frame.payload_length))
                websocket_close(client, 1002);
            else
                websocket_control(client, 8, payload, frame.payload_length);
            client.rx_length = 0;
            return;
        }
        if (frame.opcode == 9) {
            websocket_control(client, 10, payload, frame.payload_length);
            continue;
        }
        if (frame.opcode == 10)
            continue;
        if (frame.opcode == 2) {
            websocket_close(client, 1003);
            client.rx_length = 0;
            return;
        }
        if (frame.opcode == 1 && client.fragment_opcode) {
            websocket_close(client, 1002);
            client.rx_length = 0;
            return;
        }
        if (frame.opcode == 0 && !client.fragment_opcode) {
            websocket_close(client, 1002);
            client.rx_length = 0;
            return;
        }
        if (frame.opcode == 1 && frame.final) {
            if (!WebProtocol::valid_utf8(payload, frame.payload_length)) {
                websocket_close(client, 1007);
                client.rx_length = 0;
                return;
            }
            client.core_frame_length = static_cast<uint32_t>(frame.header_length + frame.payload_length);
            client.core_fragmented = false;
            if (!dispatch_message(client, payload, frame.payload_length, client.rx_received_at)) {
                // Keep the original wire frame for the next poll. New bytes
                // may follow it, but can neither overtake nor duplicate it.
                WebProtocol::unmask(payload, frame.payload_length, frame.mask);
                consumed -= frame.header_length + frame.payload_length;
                break;
            }
        } else {
            if (frame.opcode == 1) {
                client.fragment_opcode = 1;
                client.fragment_received_at = client.rx_received_at;
            }
            if (client.fragment_length + frame.payload_length > sizeof(client.fragment) - 1) {
                websocket_close(client, 1009);
                client.rx_length = 0;
                return;
            }
            std::memcpy(client.fragment + client.fragment_length, payload, frame.payload_length);
            client.fragment_length += frame.payload_length;
            if (frame.final) {
                if (!WebProtocol::valid_utf8(client.fragment, client.fragment_length)) {
                    websocket_close(client, 1007);
                    client.rx_length = 0;
                    return;
                }
                // Fragment timestamp remains pinned to the first fragment so
                // a delayed continuation cannot refresh an old sample.
                client.core_frame_length = static_cast<uint32_t>(frame.header_length + frame.payload_length);
                client.core_fragmented = true;
                if (!dispatch_message(client, client.fragment, client.fragment_length,
                                      client.fragment_received_at)) {
                    // The preceding fragments are already retained. Undo only
                    // this final append before replaying its masked wire frame.
                    client.fragment_length -= frame.payload_length;
                    WebProtocol::unmask(payload, frame.payload_length, frame.mask);
                    consumed -= frame.header_length + frame.payload_length;
                    break;
                }
                client.fragment_length = 0;
                client.fragment_opcode = 0;
            }
        }
    }
    if (client.handle && consumed) {
        client.rx_length -= consumed;
        std::memmove(client.rx, client.rx + consumed, client.rx_length);
    }
}

unsigned active_clients() {
    unsigned count = 0;
    for (const auto &client : clients)
        if (client.handle)
            ++count;
    return count;
}

size_t network_json(char *output, size_t capacity) {
    const uint32_t flags = runtime_bits.load(std::memory_order_relaxed);
    const auto transport = Transport::status();
    const auto led = StatusLed::snapshot();
    // A compact, read-only diagnostic distinguishes lost HCI/HID reports from
    // a RemoteControl mailbox/calibration or Xbox USB output problem.
#if defined(CONFIG_EN_BLUETOOTH)
    char bt_flow[1280]{};
#else
    char bt_flow[1]{};
#endif
#if defined(CONFIG_EN_BLUETOOTH)
    const auto bt = bluepad32::hid_flow_stats(to_ms_since_boot(get_absolute_time()));
    const int bt_n = std::snprintf(bt_flow, sizeof(bt_flow),
                  "\"bt_hid\":{\"ready\":%lu,\"disconnects\":%lu,\"callbacks\":%lu,"
                  "\"reports\":%lu,\"age_ms\":%lu,\"slot\":%ld,\"output\":%ld,"
                  "\"type\":%lu,\"hids_cid\":%lu,\"le\":%s,"
                  "\"name\":\"%s\",\"vid\":%lu,\"pid\":%lu,"
                  "\"stage\":%lu,\"stage_age_ms\":%lu,\"ready_age_ms\":%lu,"
                  "\"failure\":%lu,\"error_code\":%lu,\"failure_age_ms\":%lu,"
                  "\"pairing_state\":%lu,\"pairing_status\":%lu,\"pairing_reason\":%lu,"
                  "\"hci_reason\":%lu,"
                  "\"hids\":{\"setup\":%lu,\"cccd_try\":%lu,\"cccd_ack\":%lu,"
                  "\"cccd_ok\":%lu,\"cccd_errors\":%lu,\"cccd_req_status\":%lu,"
                  "\"cccd_status\":%lu,\"notify\":%lu,\"get_reply\":%lu,"
                  "\"raw_notify\":%lu,\"raw_value_handle\":%lu,\"raw_age_ms\":%lu,"
                  "\"origin\":%lu,\"report_id\":%lu,\"notify_age_ms\":%lu,"
                  "\"get_age_ms\":%lu,\"exit_calls\":%lu,\"exit_status\":%lu,"
                  "\"get_try\":%lu,\"get_rejected\":%lu,\"get_status\":%lu}},",
                  static_cast<unsigned long>(bt.ready_events),
                  static_cast<unsigned long>(bt.disconnect_events),
                  static_cast<unsigned long>(bt.callbacks),
                  static_cast<unsigned long>(bt.gamepad_reports),
                  static_cast<unsigned long>(bt.last_gamepad_age_ms),
                  static_cast<long>(bt.bt_slot), static_cast<long>(bt.output_slot),
                  static_cast<unsigned long>(bt.controller_type),
                  static_cast<unsigned long>(bt.hids_cid), bt.bluetooth_le ? "true" : "false",
                  bt.name, static_cast<unsigned long>(bt.vendor_id),
                  static_cast<unsigned long>(bt.product_id),
                  static_cast<unsigned long>(bt.stage),
                  static_cast<unsigned long>(bt.stage_age_ms),
                  static_cast<unsigned long>(bt.ready_age_ms),
                  static_cast<unsigned long>(bt.failure_kind),
                  static_cast<unsigned long>(bt.failure_code),
                  static_cast<unsigned long>(bt.failure_age_ms),
                  static_cast<unsigned long>(bt.pairing_state),
                  static_cast<unsigned long>(bt.pairing_status),
                  static_cast<unsigned long>(bt.pairing_reason),
                  static_cast<unsigned long>(bt.disconnect_reason),
                  static_cast<unsigned long>(bt.hids_setup_status),
                  static_cast<unsigned long>(bt.cccd_attempts),
                  static_cast<unsigned long>(bt.cccd_acks),
                  static_cast<unsigned long>(bt.cccd_ok),
                  static_cast<unsigned long>(bt.cccd_errors),
                  static_cast<unsigned long>(bt.cccd_request_status),
                  static_cast<unsigned long>(bt.cccd_last_status),
                  static_cast<unsigned long>(bt.notifications),
                  static_cast<unsigned long>(bt.get_results),
                  static_cast<unsigned long>(bt.raw_gatt_notifications),
                  static_cast<unsigned long>(bt.raw_gatt_value_handle),
                  static_cast<unsigned long>(bt.raw_gatt_age_ms),
                  static_cast<unsigned long>(bt.last_origin),
                  static_cast<unsigned long>(bt.last_report_id),
                  static_cast<unsigned long>(bt.notification_age_ms),
                  static_cast<unsigned long>(bt.get_result_age_ms),
                  static_cast<unsigned long>(bt.exit_suspend_calls),
                  static_cast<unsigned long>(bt.exit_suspend_status),
                  static_cast<unsigned long>(bt.get_requests),
                  static_cast<unsigned long>(bt.get_rejected),
                  static_cast<unsigned long>(bt.get_request_status));
    if (bt_n < 0 || static_cast<size_t>(bt_n) >= sizeof(bt_flow)) return 0;
#endif
#if defined(CONFIG_OGXM_BOARD_PI_PICOW_2023)
    constexpr const char *board = "pico_w_2023";
#elif defined(CONFIG_OGXM_BOARD_PI_PICO2W)
    constexpr const char *board = "pico2_w";
#else
    constexpr const char *board = "pico_w";
#endif
#if defined(CONFIG_EN_USB_HOST) || defined(CONFIG_EN_BLUETOOTH)
    constexpr const char *physical_supported = "true";
#else
    constexpr const char *physical_supported = "false";
#endif
    const char *delivery = transport.delivery == Transport::Delivery::PeerAcknowledged
                               ? "peer_acknowledged"
                               : "module_accepted";
    const int n = std::snprintf(
        output, capacity,
        "{\"ready\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"channel\":%u,"
        "\"clients\":%u,\"requests\":%lu,\"usb_configured\":%s,"
        "\"bluetooth_connected\":%s,%s\"wired_connected\":%s,\"remote_owned\":%s,\"board\":\"%s\","
        "\"bluetooth_available\":%s,\"physical_supported\":%s,\"transport\":\"%s\",\"uart_baud\":%lu,"
        "\"input_hz\":%u,\"save_delivery\":\"%s\",\"error\":\"%s\","
        "\"ap_ip\":\"%s\",\"ap_active\":%s,\"station_configured\":%s,\"station_connected\":%s,"
        "\"station_joining\":%s,\"station_retry_pending\":%s,\"station_waiting_for\":\"%s\","
        "\"station_ip\":\"%s\",\"hostname\":\"%s\",\"mdns_available\":%s,"
        "\"control_ready\":%s,\"control_waiting_for\":\"%s\",\"mdns_state\":\"%s\",\"mdns_error\":\"%s\","
        "\"mdns_attempts\":%lu,\"command_timeouts\":%lu,\"modem_firmware\":\"%s\","
        "\"station_error\":\"%s\",\"station_reconnects\":%lu,\"recoveries\":%lu,"
        "\"udp_discovery\":\"%s\",\"udp_beacons\":%lu,"
        "\"led\":{\"available\":%s,\"pin\":%d,\"state\":\"%s\",\"profile_id\":%u,"
        "\"profile_color\":\"#%06lX\",\"output_color\":\"#%06lX\",\"brightness\":%u,"
        "\"enabled\":%s,\"mode\":\"%s\",\"applied_revision\":%lu}}",
        transport.ready ? "true" : "false", ap_ssid,
        transport.station_connected ? transport.station_ip : transport.ap_ip,
        OGXM_WIFI_CHANNEL, active_clients(),
        static_cast<unsigned long>(request_count), flags & 1 ? "true" : "false",
        flags & 2 ? "true" : "false", bt_flow, flags & 4 ? "true" : "false", flags & 8 ? "true" : "false", board,
        transport.bluetooth_available ? "true" : "false", physical_supported, transport.name,
        static_cast<unsigned long>(transport.uart_baud),
        static_cast<unsigned>(transport.recommended_input_hz), delivery, transport.error,
        transport.ap_ip, transport.ap_active ? "true" : "false",
        transport.station_configured ? "true" : "false",
        transport.station_connected ? "true" : "false",
        transport.station_joining ? "true" : "false",
        transport.station_retry_pending || retry_queued ? "true" : "false", station_waiting_for(transport), transport.station_ip,
        transport.hostname, transport.mdns_available ? "true" : "false",
        transport.control_ready ? "true" : "false", transport.control_waiting_for,
        transport.mdns_state, transport.mdns_error,
        static_cast<unsigned long>(transport.mdns_attempts),
        static_cast<unsigned long>(transport.command_timeouts), transport.modem_firmware,
        transport.station_error,
        static_cast<unsigned long>(transport.station_reconnects),
        static_cast<unsigned long>(transport.recoveries),
        transport.udp_discovery_state,
        static_cast<unsigned long>(transport.udp_beacons),
        StatusLed::available() ? "true" : "false", StatusLed::pin(), StatusLed::state_name(led.phase),
        static_cast<unsigned>(led.profile_id), static_cast<unsigned long>(led.profile_color),
        static_cast<unsigned long>(led.output_color), static_cast<unsigned>(led.brightness),
        led.enabled ? "true" : "false", LedSettings::mode_name(led.mode),
        static_cast<unsigned long>(led.applied_revision));
    return n > 0 && static_cast<size_t>(n) < capacity ? static_cast<size_t>(n) : 0;
}

void dispatch_http(Client &client, const WebProtocol::Request &request) {
    RefreshTransportGuard guard;
    if (!client.dispatch_deferred) {
        ++request_count;
        client.dispatch_started = now_ms();
    }
    if (!WebProtocol::same_origin(request)) {
        error_reply(client, 403, "origin_not_allowed");
        return;
    }
    if (!std::strcmp(request.path, "/ws")) {
        char accept[29];
        if (!request.websocket || !WebProtocol::websocket_accept(request.websocket_key, accept)) {
            error_reply(client, 400, "websocket_upgrade_required");
            return;
        }
        // A monitoring UI and a Python sender may both keep a socket open.
        // RemoteControl still grants one input lease, and one TCP slot remains
        // available for short HTTP status/tuning requests.
        unsigned streams = 0;
        for (const auto &other : clients)
            if (&other != &client && other.handle && other.websocket)
                ++streams;
        if (streams >= 2) {
            error_reply(client, 429, "stream_limit_reached");
            return;
        }
        const int n =
            std::snprintf(client.response_header, sizeof(client.response_header),
                          "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                          "Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
                          accept);
        client.header_length = static_cast<size_t>(n);
        client.body_length = 0;
        client.response_body = nullptr;
        client.response_pending = true;
        client.websocket = true;
        client.upgrade_pending = true;
        client.close_after_send = false;
        client.queued = client.completed = 0;
        client.all_peer_acknowledged = true;
        const size_t used = request.header_length + request.body_length;
        client.rx_length -= used;
        std::memmove(client.rx, client.rx + used, client.rx_length);
        pump(client);
        return;
    }
    if (!std::strcmp(request.method, "GET") &&
        (!std::strcmp(request.path, "/") || !std::strcmp(request.path, "/index.html"))) {
        reply(client, 200, ogxm_web_index_gzip, ogxm_web_index_gzip_length,
              "text/html; charset=utf-8", true);
        return;
    }
    if (!std::strcmp(request.method, "GET") && !std::strcmp(request.path, "/favicon.ico")) {
        reply(client, 204, nullptr, 0);
        return;
    }
    if (std::strcmp(request.method, "GET") && std::strcmp(request.method, "POST")) {
        error_reply(client, 405, "method_not_allowed");
        return;
    }
    if (!std::strcmp(request.method, "POST") && !request.json) {
        error_reply(client, 415, "application_json_required");
        return;
    }
    if (!std::strcmp(request.method, "GET") && !std::strcmp(request.path, "/api/network")) {
        const size_t length = network_json(client.response, sizeof(client.response));
        if (!length) { error_reply(client, 500, "response_too_large"); return; }
        reply(client, 200, reinterpret_cast<const uint8_t *>(client.response), length);
        return;
    }
    if (!std::strcmp(request.method, "GET") && !std::strcmp(request.path, "/api/network/scan")) {
        const size_t length = scan_json(client.response, sizeof(client.response));
        if (!length) { error_reply(client, 500, "response_too_large"); return; }
        reply(client, 200, reinterpret_cast<const uint8_t*>(client.response), length);
        return;
    }
    const char *body = reinterpret_cast<const char *>(client.rx) + request.header_length;
    const bool post = !std::strcmp(request.method, "POST");
    const bool update_route = !std::strcmp(request.path, "/api/update") ||
                              !std::strncmp(request.path, "/api/update/", 12);
    // Only one operation may own the response-before-reboot path. An updater
    // stages flash while its maintenance lock excludes profile/network saves.
    if (post && pending_action.load(std::memory_order_acquire) != SaveAction::None) {
        error_reply(client, 409, "save_pending");
        return;
    }
    if (post && !update_route && FirmwareUpdate::active() &&
        (!std::strncmp(request.path, "/api/profile", 12) ||
         !std::strcmp(request.path, "/api/calibration") ||
         !std::strcmp(request.path, "/api/control/mode") ||
         !std::strncmp(request.path, "/api/led", 8) ||
         !std::strncmp(request.path, "/api/oled", 9) ||
         !std::strncmp(request.path, "/api/network/", 13))) {
        error_reply(client, 423, "firmware_update_active");
        return;
    }
    const bool configuration_mutation = post &&
        ((!std::strncmp(request.path, "/api/network/", 13) &&
          std::strcmp(request.path, "/api/network/mdns/retry")) ||
         !std::strncmp(request.path, "/api/led/", 9) ||
         !std::strncmp(request.path, "/api/oled/", 10) ||
         (update_route && std::strcmp(request.path, "/api/update/cancel")));
    if (configuration_mutation && !RemoteControl::configuration_allowed()) {
        // On Bluetooth boards settings work explicitly isolates console input.
        // Wi-Fi-only boards keep their Wi-Fi policy. Cancellation and the
        // deferred, input-neutral mDNS diagnostic remain available for recovery.
        error_reply(client, 409, "configuration_mode_required");
        return;
    }
    if (!std::strcmp(request.path, "/api/network/scan")) {
        const auto transport = Transport::status();
        if (!transport.ready) { error_reply(client, 503, "network_not_ready"); return; }
        const auto& scan = Transport::scan_status();
        const bool backend_busy = scan.phase == WifiScan::Phase::Queued || scan.phase == WifiScan::Phase::Running;
        const uint32_t generation = scan_queued ? scan_requested_generation : backend_busy ?
            scan.generation : next_scan_generation(scan.generation);
        client.scan_response = !scan_queued && !backend_busy;
        client.scan_generation = generation;
        const int length = std::snprintf(client.response, sizeof(client.response),
            "{\"ok\":true,\"scan_pending\":true,\"generation\":%lu}", static_cast<unsigned long>(generation));
        reply(client, 202, reinterpret_cast<const uint8_t*>(client.response), static_cast<size_t>(length));
        return;
    }
    if (!std::strcmp(request.path, "/api/network/retry")) {
        if (!post) { error_reply(client, 405, "method_not_allowed"); return; }
        const auto state = Transport::status();
        if (!state.station_configured) {
            error_reply(client, 409, "station_not_configured");
            return;
        }
        if (state.station_connected) {
            static constexpr char connected[] = "{\"ok\":true,\"already_connected\":true}";
            reply(client, 200, reinterpret_cast<const uint8_t*>(connected), sizeof(connected) - 1);
            return;
        }
        // Publish only after the entire response drains. A dropped reply must
        // never start an unexpected radio scan; repeated requests coalesce.
        client.retry_response = !retry_queued && !state.station_retry_pending && !state.station_joining;
        static constexpr char queued[] = "{\"ok\":true,\"retry_pending\":true}";
        reply(client, 202, reinterpret_cast<const uint8_t*>(queued), sizeof(queued) - 1);
        return;
    }
    if (!std::strcmp(request.path, "/api/network/mdns/retry")) {
        if (!post) { error_reply(client, 405, "method_not_allowed"); return; }
        const auto state = Transport::status();
        if (!state.ready) { error_reply(client, 503, "network_not_ready"); return; }
        if (state.station_configured && (!state.station_connected || !state.station_ip[0])) {
            error_reply(client, 409, "station_not_connected");
            return;
        }
        // ESP native mDNS is published on its station interface; the official
        // board can also advertise on its own AP through lwIP.
        if (!state.station_configured && !state.bluetooth_available) {
            error_reply(client, 409, "station_not_configured");
            return;
        }
        client.mdns_retry_response = !mdns_retry_queued;
        static constexpr char queued[] = "{\"ok\":true,\"mdns_pending\":true}";
        reply(client, 202, reinterpret_cast<const uint8_t*>(queued), sizeof(queued) - 1);
        return;
    }
    if (!std::strcmp(request.path, "/api/led") || !std::strncmp(request.path, "/api/led/", 9)) {
        LedSettings::Result result{};
        if (!post && !std::strcmp(request.path, "/api/led"))
            result = LedSettings::public_json(client.response, sizeof(client.response));
        else if (post && !std::strcmp(request.path, "/api/led/apply"))
            result = LedSettings::apply_json(body, request.body_length, client.response, sizeof(client.response));
        else if (post && !std::strcmp(request.path, "/api/led/save"))
            result = LedSettings::stage_json(body, request.body_length, client.response, sizeof(client.response));
        else if (post && !std::strcmp(request.path, "/api/led/test"))
            result = LedSettings::test_json(body, request.body_length, client.response, sizeof(client.response));
        else { error_reply(client, 404, "not_found"); return; }
        if (defer_busy(client, result.status, client.response, result.length)) return;
        if (!post && result.status == 200 && !std::strcmp(request.path, "/api/led"))
            OledDisplay::notify(OledDisplay::Activity::LedLoad);
        if (result.status >= 200 && result.status < 300 && post) {
            if (!std::strcmp(request.path, "/api/led/apply"))
                OledDisplay::notify(OledDisplay::Activity::LedApply);
            else if (result.save_response)
                OledDisplay::notify(OledDisplay::Activity::LedSave);
        }
        if (result.save_response) {
            client.save_action = SaveAction::Led;
            pending_action.store(SaveAction::Led, std::memory_order_release);
        }
        reply(client, result.status, reinterpret_cast<const uint8_t*>(client.response),
              result.length, "application/json", false, result.save_response);
        return;
    }
    if (!std::strcmp(request.path, "/api/oled") || !std::strncmp(request.path, "/api/oled/", 10)) {
        OledSettings::Result result{};
        if (!post && !std::strcmp(request.path, "/api/oled"))
            result=OledSettings::public_json(client.response,sizeof(client.response));
        else if (post && !std::strcmp(request.path, "/api/oled/apply"))
            result=OledSettings::apply_json(body,request.body_length,client.response,sizeof(client.response));
        else if (post && !std::strcmp(request.path, "/api/oled/save"))
            result=OledSettings::stage_json(body,request.body_length,client.response,sizeof(client.response));
        else { error_reply(client,404,"not_found"); return; }
        if (defer_busy(client,result.status,client.response,result.length)) return;
        if (result.status >= 200 && result.status < 300 && post) {
            OledDisplay::notify(result.save_response ? OledDisplay::Activity::OledSave : OledDisplay::Activity::OledApply);
        }
        if (result.save_response) {
            client.save_action=SaveAction::Oled;
            pending_action.store(SaveAction::Oled,std::memory_order_release);
        }
        reply(client,result.status,reinterpret_cast<const uint8_t*>(client.response),
              result.length,"application/json",false,result.save_response);
        return;
    }
    if (!std::strcmp(request.path, "/api/network/config")) {
        const auto result = post ? NetworkSettings::stage_json(body, request.body_length,
                                      client.response, sizeof(client.response))
                                 : NetworkSettings::public_json(client.response, sizeof(client.response));
        if (!post && result.status == 200)
            OledDisplay::notify(OledDisplay::Activity::NetworkLoad);
        if (result.save_response)
            OledDisplay::notify(OledDisplay::Activity::NetworkSave);
        if (result.save_response) {
            client.save_action = SaveAction::Network;
            pending_action.store(SaveAction::Network, std::memory_order_release);
        }
        reply(client, result.status, reinterpret_cast<const uint8_t *>(client.response),
              result.length, "application/json", false, result.save_response);
        return;
    }
    if (update_route) {
        const auto result = FirmwareUpdate::handle_request(request.method, request.path, body,
            request.body_length, client.response, sizeof(client.response), now_ms());
        if (defer_busy(client, result.status, client.response, result.length)) return;
        if (post && result.status >= 200 && result.status < 300 &&
            !std::strcmp(request.path, "/api/update/cancel"))
            OledDisplay::notify(OledDisplay::Activity::FirmwareCancel);
        if (result.install_response) {
            client.save_action = SaveAction::Firmware;
            pending_action.store(SaveAction::Firmware, std::memory_order_release);
        }
        reply(client, result.status, reinterpret_cast<const uint8_t *>(client.response),
              result.length, "application/json", false, result.install_response);
        return;
    }
    const bool input_sample = !std::strcmp(request.path, "/api/control/input") ||
                              !std::strcmp(request.path, "/api/control/heartbeat");
    if (post && !std::strcmp(request.path, "/api/control/acquire") &&
        !Transport::status().control_ready) {
        // Reject before touching the lease. Initial station/mDNS setup and
        // an already-running AT join cannot coexist with fresh input traffic.
        error_reply(client, 503, "network_initializing");
        return;
    }
    // Consume controller/profile/status HTTP work on Core0. The original
    // request stays pinned in client.rx until Core0 acknowledges completion.
    if (client.core_pending) return;
    if (!RemoteControl::submit_core_request(client_slot(client), false, request.method,
            request.path, body, request.body_length, client.response,
            sizeof(client.response), input_sample ? client.dispatch_started : now_ms())) {
        // Bounded backpressure: defer on the next service poll, never reject a
        // valid request just because Core0 is in the middle of a USB frame.
        client.dispatch_deferred = true;
        client.dispatch_due = now_ms() + SERVICE_RETRY_MS;
        return;
    }
    client.core_pending = true;
    client.core_websocket = false;
    client.core_post = post;
    std::strncpy(client.core_path, request.path, sizeof(client.core_path) - 1);
    client.core_path[sizeof(client.core_path) - 1] = '\0';
    return;
}

// Either radio transport and Core0 communicate through a fixed completion
// slot. This finalizer runs on the radio poll, never during the UART callback.
void complete_core_requests() {
    for (auto& client : clients) {
        if (!client.core_pending) continue;
        RemoteControl::HttpResult result{};
        if (!RemoteControl::poll_core_result(client_slot(client), result)) continue;
        client.core_pending = false;
        if (client.core_abandoned || client.transport_closing || !client.handle) {
            // A disconnect may have raced an acquire. Clear the accepted token
            // before releasing this client's buffer generation for reuse.
            if (client.core_websocket && client.core_token && result.status < 400)
                client.control_token = client.core_token;
            if (!client.core_websocket && !std::strcmp(client.core_path, "/api/control/acquire") &&
                result.status == 200) {
                constexpr char marker[] = "\"token\":";
                const char* token = std::strstr(client.response, marker);
                if (token) {
                    token += sizeof(marker) - 1;
                    uint32_t value = 0;
                    while (*token >= '0' && *token <= '9') {
                        value = value * 10u + static_cast<uint32_t>(*token++ - '0');
                    }
                    if (value) RemoteControl::release_token(value, now_ms());
                }
            }
            forget(client);
            continue;
        }
        if (client.core_websocket) {
            if (client.core_token && result.status < 400)
                client.control_token = client.core_token;
            if (client.core_frame_length <= client.rx_length) {
                const size_t count = client.core_frame_length;
                client.rx_length -= count;
                std::memmove(client.rx, client.rx + count, client.rx_length);
            } else client.rx_length = 0;
            client.core_frame_length = 0;
            if (client.core_fragmented) {
                client.fragment_length = 0;
                client.fragment_opcode = 0;
            }
            client.core_fragmented = false;
            client.dispatch_deferred = false;
            if ((result.status >= 400 || uint32_t(now_ms() - client.last_ack) >= 500) &&
                !client.response_pending && result.length <= sizeof(client.core_reply)) {
                websocket_send(client, 1,
                    reinterpret_cast<const uint8_t*>(client.core_reply), result.length);
                client.last_ack = now_ms();
            }
            // Core0 may have completed a frame while more TCP bytes arrived.
            // The stream must advance even if an earlier low-rate ACK is still
            // queued for transmission; outstanding output is not input flow
            // control, and otherwise buffered frames can remain stuck forever.
            if (client.rx_length && !client.closing && !client.transport_closing)
                process_websocket(client);
        } else {
            // Preserve the existing response, notification and deferred-save
            // semantics after the request has been consumed exactly once.
            if (!client.core_post && !std::strcmp(client.core_path, "/api/status") &&
                result.status == 200 && result.length &&
                result.length < sizeof(client.response) && client.response[result.length - 1] == '}') {
                constexpr char prefix[] = ",\"network\":";
                const size_t begin = result.length - 1;
                const size_t rest = sizeof(client.response) - begin;
                if (rest <= sizeof(prefix) + 1) { error_reply(client, 500, "response_too_large"); continue; }
                std::memcpy(client.response + begin, prefix, sizeof(prefix) - 1);
                const size_t length = network_json(client.response + begin + sizeof(prefix) - 1,
                    rest - sizeof(prefix));
                if (!length || begin + sizeof(prefix) - 1 + length + 1 >= sizeof(client.response)) {
                    error_reply(client, 500, "response_too_large"); continue;
                }
                result.length = begin + sizeof(prefix) - 1 + length;
                client.response[result.length++] = '}';
                client.response[result.length] = 0;
            }
            if (!client.core_post && result.status == 200 &&
                (!std::strcmp(client.core_path, "/api/profile") ||
                 !std::strcmp(client.core_path, "/api/profiles")))
                OledDisplay::notify(OledDisplay::Activity::ProfileLoad);
            if (client.core_post && result.status >= 200 && result.status < 300) {
                if (!std::strcmp(client.core_path, "/api/profile/select"))
                    OledDisplay::notify(OledDisplay::Activity::ProfileLoad);
                else if (!std::strcmp(client.core_path, "/api/profile") && result.status == 202)
                    OledDisplay::notify(OledDisplay::Activity::ProfileApply);
                else if (result.save_response)
                    OledDisplay::notify(OledDisplay::Activity::ProfileSave);
            }
            if (result.save_response) {
                client.save_action = SaveAction::Profile;
                pending_action.store(SaveAction::Profile, std::memory_order_release);
            }
            if (result.length > sizeof(client.response)) {
                error_reply(client, 500, "response_too_large"); continue;
            }
            reply(client, result.status,
                reinterpret_cast<const uint8_t*>(client.response), result.length,
                "application/json", false, result.save_response);
        }
    }
}

void on_receive(Transport::Handle handle, const uint8_t *bytes, size_t length) {
    Client *client = find_client(handle);
    if (!client || client->transport_closing || !length)
        return;
    client->last_activity = now_ms();
    // A Core0 RPC owns the current request, but incoming WebSocket bytes are
    // still part of a continuous TCP stream. Do not discard them: the next
    // frame may arrive (or be split across UART callbacks) before completion.
    // Core0 copies WebSocket JSON into its own fixed slot, so appending later
    // bytes does not mutate a buffer borrowed by Core0. HTTP bodies, in
    // contrast, are borrowed from client.rx and must remain untouched.
    if (client->core_pending && !client->websocket) return;
    if (client->closing || client->close_after_send ||
        (!client->websocket && (client->response_pending || client->dispatch_deferred)))
        return;
    if (length > RX_SIZE - client->rx_length) {
        if (client->websocket)
            websocket_close(*client, 1009);
        else
            error_reply(*client, 413, "request_too_large");
        return;
    }
    // WebSocket frames can be split across TCP deliveries. Once the final bytes
    // arrive, the complete input sample is fresh even when one byte of the next
    // frame has kept this buffer nonempty. This previously caused continuous
    // controller traffic to inherit a timestamp from seconds ago.
    // Do NOT re-date a fully received frame held for a service_busy retry: its
    // age must stay tied to its original arrival.
    if (!client->rx_length || (client->websocket && !client->dispatch_deferred))
        client->rx_received_at = now_ms();
    std::memcpy(client->rx + client->rx_length, bytes, length);
    client->rx_length += length;
    client->rx[client->rx_length] = 0;
    if (client->websocket) {
        // Leave all parsing/consumption until Core0 completes the request
        // pinned at the front of rx. Received bytes remain buffered in order.
        if (!client->upgrade_pending && !client->dispatch_deferred
            && !client->core_pending
        ) process_websocket(*client);
    } else {
        WebProtocol::Request request;
        const auto parsed = WebProtocol::parse_request(client->rx, client->rx_length, request);
        if (parsed == WebProtocol::ParseResult::Complete)
            dispatch_http(*client, request);
        else if (parsed == WebProtocol::ParseResult::Invalid)
            error_reply(*client, 400, "invalid_http_request");
        else if (parsed == WebProtocol::ParseResult::TooLarge)
            error_reply(*client, 413, "request_too_large");
    }
}

void on_read_closed(Transport::Handle handle) {
    if (Client *client = find_client(handle)) {
        if (client->core_pending) {
            client->core_abandoned = true;
            RemoteControl::abandon_core_request(client_slot(*client));
        } else
        release_control(*client);
        client->dispatch_deferred = false;
        client->rx_length = client->fragment_length = 0;
        // A peer FIN can arrive before its ACK of a complete HTTP reply.
        // Preserve that response; an incomplete request can close immediately.
        client->close_after_send = true;
        if (!client->response_pending)
            close_client(*client);
    }
}

void on_sent(Transport::Handle handle, size_t length, Transport::Delivery delivery) {
    Client *client = find_client(handle);
    if (!client || client->transport_closing)
        return;
    // A stale callback may not complete a new response or a reused connection.
    if (!client->response_pending || length > client->queued - client->completed) {
        abort_client(*client);
        return;
    }
    client->last_activity = now_ms();
    client->completed += static_cast<uint32_t>(length);
    if (delivery != Transport::Delivery::PeerAcknowledged)
        client->all_peer_acknowledged = false;
    const size_t total = client->header_length + client->body_length;
    if (client->completed >= total && client->queued >= total) {
        client->response_pending = false;
        const uint8_t save = client->save_response
            ? ((client->all_peer_acknowledged ? 3 : 1) |
               (static_cast<uint8_t>(client->save_action) << 2)) : 0;
        if (save)
            client->save_completed = true;
        const bool retry = client->retry_response;
        client->retry_response = false;
        if (retry && !retry_queued) {
            retry_queued = true;
            retry_due = now_ms() + 250;
        }
        if (client->mdns_retry_response && !mdns_retry_queued) {
            mdns_retry_queued = true;
            mdns_retry_due = now_ms() + 250;
        }
        client->mdns_retry_response = false;
        if (client->scan_response && !scan_queued &&
            client->scan_generation != Transport::scan_status().generation) {
            scan_queued = true;
            scan_dispatch_failed = false;
            scan_requested_generation = client->scan_generation;
            scan_due = now_ms() + 250;
        }
        client->scan_response = false;
        if (client->close_after_send) {
            close_client(*client);
            // Publish after initiating close; core0 may stop this core to save.
            if (save)
                save_completion.store(save, std::memory_order_release);
            return;
        }
        if (save)
            save_completion.store(save, std::memory_order_release);
        if (client->control_opcode) {
            const uint8_t opcode = client->control_opcode;
            client->control_opcode = 0;
            websocket_send(*client, opcode, client->control_payload, client->control_length,
                           opcode == 8);
        }
        if (!client->response_pending && client->pong_pending) {
            client->pong_pending = false;
            application_pong(*client, client->pong_id);
        }
        if (client->upgrade_pending) {
            client->upgrade_pending = false;
            if (client->rx_length)
                process_websocket(*client);
        }
    } else {
        pump(*client);
    }
}

void on_closed(Transport::Handle handle) {
    if (Client *client = find_client(handle))
        forget(*client);
}

bool on_connected(Transport::Handle handle) {
    if (!handle || find_client(handle))
        return false;
    for (auto &client : clients) {
        if (client.handle)
            continue;
        if (RemoteControl::core_request_busy(client_slot(client))) continue;
        // Assigning a value-initialized Client can materialize a >11 KiB stack
        // temporary. Clear this fixed workspace directly instead.
        std::memset(static_cast<void *>(&client), 0, sizeof(client));
        client.handle = handle;
        client.last_activity = now_ms();
        client.all_peer_acknowledged = true;
        return true;
    }
    return false;
}

const Transport::Callbacks callbacks{on_connected, on_receive, on_sent, on_closed, on_read_closed};
} // namespace

bool start() {
    if (started)
        return true;
    static_assert(sizeof(OGXM_WIFI_PASSWORD) >= 9 && sizeof(OGXM_WIFI_PASSWORD) <= 64,
                  "SoftAP password must contain 8..63 characters");
    pico_get_unique_board_id_string(device_id, sizeof(device_id));
    std::snprintf(ap_ssid, sizeof(ap_ssid), "%s-%s", OGXM_WIFI_SSID_PREFIX,
                  device_id + std::strlen(device_id) - 4);
    const auto& settings = NetworkSettings::get();
    started = Transport::start({ap_ssid, OGXM_WIFI_PASSWORD, OGXM_WIFI_CHANNEL,
        settings.station_enabled ? settings.station_ssid : "", settings.station_password,
        settings.hostname, device_id}, callbacks);
    service_ready.store(Transport::status().ready, std::memory_order_release);
    if (started) {
        std::printf("dc-ogx-w: starting %s; join %s and open http://192.168.4.1\n",
                    Transport::status().name, ap_ssid);
    } else {
        std::printf("dc-ogx-w: network start failed; local controller paths remain available\n");
    }
    return started;
}

void poll() {
    // Handoff acknowledgement must not wait behind modem command processing,
    // stale TCP retries or an HTTP client that has resumed after long idle.
    // Runs on the serialized radio core, never a USB interrupt/callback.
    RemoteControl::apply_profile_on_radio_core();
    refresh_transport_guard();
    Transport::poll(now_ms());
    // Parse requests and format responses on the radio core. Core0 exchanges
    // validated controller state and snapshots; it never executes HTTP/JSON.
    RemoteControl::service_network_requests();
    complete_core_requests();
    // Both radio backends publish snapshots; OLED I2C belongs to Core0.
    publish_oled_network(Transport::status());
    // Cancel both in-flight reply completions and Core0's settling timer before
    // reopening the gate. Otherwise an old delayed install could arm a newly
    // uploaded image after a recoverable pre-install validation failure.
    if (pending_action.load(std::memory_order_acquire) == SaveAction::Firmware &&
        FirmwareUpdate::failed() && !FirmwareUpdate::active()) {
        const uint8_t cancellation = install_cancel_state.load(std::memory_order_acquire);
        if (cancellation == 0) {
            for (auto& client : clients) {
                if (client.save_action == SaveAction::Firmware) {
                    client.save_response = false;
                    client.save_action = SaveAction::None;
                }
            }
            save_completion.store(0, std::memory_order_release);
            install_cancel_state.store(1, std::memory_order_release);
        } else if (cancellation == 2) {
            install_cancel_state.store(0, std::memory_order_release);
            pending_action.store(SaveAction::None, std::memory_order_release);
        }
    }
    const auto transport = Transport::status();
    const bool station_failed = transport.station_configured && !transport.station_connected &&
        !transport.station_joining && transport.station_error[0] &&
        std::strcmp(transport.station_error, "station_starting") &&
        std::strcmp(transport.station_error, "station_waiting_for_address") &&
        std::strcmp(transport.station_error, "station_disconnected");
    const bool radio_error = transport.error[0] && std::strcmp(transport.error, "esp_starting");
    service_ready.store(transport.ready, std::memory_order_release);
    health_bits.store((transport.ready ? 1u : 0u) | (transport.station_configured ? 2u : 0u) |
                      (transport.station_connected ? 4u : 0u) | (radio_error ? 8u : 0u) |
                      (station_failed ? 16u : 0u) | (transport.station_joining ? 32u : 0u) |
                      (transport.station_retry_pending || retry_queued ? 64u : 0u),
                      std::memory_order_release);
    // Receive callbacks above may have advanced last_activity by a millisecond.
    // Sample afterwards so unsigned elapsed-time arithmetic cannot underflow.
    const uint32_t now = now_ms();
    if (scan_queued && static_cast<int32_t>(now - scan_due) >= 0 &&
        pending_action.load(std::memory_order_acquire) == SaveAction::None && !FirmwareUpdate::active()) {
        scan_dispatch_failed = !Transport::request_scan();
        if (scan_dispatch_failed) scan_failed_at = now;
        // Every coalesced reply belongs to this one dispatch, even if a slow
        // peer acknowledges it after later generations have already finished.
        // Also consume peers on failure: a late ACK must not retry implicitly.
        for (auto& client : clients)
            if (client.scan_response && client.scan_generation == scan_requested_generation)
                client.scan_response = false;
        scan_queued = false;
    }
    if (retry_queued && static_cast<int32_t>(now - retry_due) >= 0 &&
        pending_action.load(std::memory_order_acquire) == SaveAction::None && !FirmwareUpdate::active()) {
        Transport::retry_connection();
        retry_queued = false;
    }
    if (mdns_retry_queued && static_cast<int32_t>(now - mdns_retry_due) >= 0 &&
        pending_action.load(std::memory_order_acquire) == SaveAction::None && !FirmwareUpdate::active()) {
        Transport::retry_mdns();
        for (auto& client : clients) client.mdns_retry_response = false;
        mdns_retry_queued = false;
    }
    for (auto &client : clients) {
        if (!client.handle || client.transport_closing)
            continue;
        const uint32_t idle = uint32_t(now - client.last_activity);
        if (idle > (client.websocket ? WS_IDLE_MS : HTTP_IDLE_MS)) {
            abort_client(client);
            continue;
        }
        if (client.dispatch_deferred && !client.closing && !client.close_after_send &&
            static_cast<int32_t>(now - client.dispatch_due) >= 0) {
            if (client.websocket) process_websocket(client);
            else {
                WebProtocol::Request request;
                if (WebProtocol::parse_request(client.rx, client.rx_length, request) ==
                    WebProtocol::ParseResult::Complete) dispatch_http(client, request);
                else abort_client(client); // A deferred HTTP request is already complete.
            }
        }
        if (!client.handle || client.transport_closing) continue;
        if (client.response_pending) {
            pump(client);
        } else if (client.close_after_send) {
            close_client(client);
        }
    }
}

void start_radio_poll() {
    Transport::start_radio_poll();
}

// Called on Core0 only. Both the official CYW43 board and the ESP8285 clone
// publish existing input ownership; the OLED never queries Bluetooth directly.
static void publish_oled_input_sources([[maybe_unused]] std::uint32_t now_ms) {
    // Read-only controller metadata. Both sources are already maintained by the
    // ordinary input pipeline; do not wake or poll the radio for OLED content.
    OledDisplay::InputKind bt_kind=OledDisplay::InputKind::Generic;
    bool bt_connected=false;
    bool bt_le=false;
#if defined(CONFIG_EN_BLUETOOTH)
    bt_connected=bluepad32::any_connected();
    if (bt_connected) {
        const auto bt=bluepad32::hid_flow_stats(now_ms);
        bt_le=bt.bluetooth_le;
        switch (bt.controller_type) {
            case CONTROLLER_TYPE_XBoxOneController: bt_kind=OledDisplay::InputKind::Xbox; break;
            case CONTROLLER_TYPE_XBox360Controller: bt_kind=OledDisplay::InputKind::Xbox360; break;
            case CONTROLLER_TYPE_PS3Controller: bt_kind=OledDisplay::InputKind::Ps3; break;
            case CONTROLLER_TYPE_PS4Controller: bt_kind=OledDisplay::InputKind::Ps4; break;
            case CONTROLLER_TYPE_PS5Controller: bt_kind=OledDisplay::InputKind::Ps5; break;
            case CONTROLLER_TYPE_SwitchProController:
            case CONTROLLER_TYPE_Switch2ProController: bt_kind=OledDisplay::InputKind::SwitchPro; break;
            case CONTROLLER_TYPE_WiiController: bt_kind=OledDisplay::InputKind::Wii; break;
            case CONTROLLER_TYPE_8BitdoController: bt_kind=OledDisplay::InputKind::EightBitDo; break;
            case CONTROLLER_TYPE_SteamController:
            case CONTROLLER_TYPE_SteamControllerTriton: bt_kind=OledDisplay::InputKind::Steam; break;
            default: break;
        }
    }
#endif
    OledDisplay::InputKind usb_kind=OledDisplay::InputKind::Generic;
    bool wired_connected=false;
#if defined(CONFIG_EN_USB_HOST)
    const auto slot=InputSlot::get(0);
    wired_connected=slot.transport==InputTransport::USB;
    if (wired_connected) {
        switch (slot.physical_driver) {
            case HostDriverType::XBOXONE: usb_kind=OledDisplay::InputKind::Xbox; break;
            case HostDriverType::XBOX360:
            case HostDriverType::XBOX360W: usb_kind=OledDisplay::InputKind::Xbox360; break;
            case HostDriverType::PS3: usb_kind=OledDisplay::InputKind::Ps3; break;
            case HostDriverType::PS4: usb_kind=OledDisplay::InputKind::Ps4; break;
            case HostDriverType::PS5: usb_kind=OledDisplay::InputKind::Ps5; break;
            case HostDriverType::SWITCH_PRO:
            case HostDriverType::SWITCH_PRO_2:
            case HostDriverType::SWITCH: usb_kind=OledDisplay::InputKind::SwitchPro; break;
            case HostDriverType::GAMESIR_CYCLONE2:
            case HostDriverType::GAMESIR_G7_PRO: usb_kind=OledDisplay::InputKind::GameSir; break;
            default: break;
        }
    }
#endif
    OledDisplay::publish_input_devices(bt_connected,bt_kind,bt_le,wired_connected,usb_kind);
}

void service_display_from_core0(std::uint32_t now_ms) {
    // Advance only the hardware FIFO between render visits; this never waits
    // for the OLED bus and does no drawing or response formatting.
    OledDisplay::poll_transfer();
    // Input metadata is cheap, but there is no reason to sample it every USB
    // iteration. Both radio backends supply the same atomic network view.
    static std::uint32_t next_sources{};
    if (static_cast<std::int32_t>(now_ms-next_sources)>=0) {
        next_sources=now_ms+100u;
        publish_oled_input_sources(now_ms);
    }

    // Both radio boards retain the OLED driver's 2-ms render/chunk cadence.
    // The bus transaction advances nonblocking above, independently of render.
    static std::uint32_t next_service{};
    if (static_cast<std::int32_t>(now_ms - next_service) < 0) return;
    next_service = now_ms + 2u;
    const std::uint32_t flags = oled_network_flags.load(std::memory_order_acquire);
    const std::uint32_t ipv4 = oled_station_ipv4.load(std::memory_order_relaxed);
    Transport::Status snapshot{};
    snapshot.ready = (flags & 1u) != 0;
    snapshot.ap_active = (flags & 2u) != 0;
    snapshot.station_configured = (flags & 4u) != 0;
    snapshot.station_connected = (flags & 8u) != 0;
    snapshot.station_joining = (flags & 16u) != 0;
    char station_ip[16]{};
    if (snapshot.station_connected && ipv4) {
        std::snprintf(station_ip, sizeof(station_ip), "%u.%u.%u.%u",
            static_cast<unsigned>((ipv4 >> 24u) & 255u),
            static_cast<unsigned>((ipv4 >> 16u) & 255u),
            static_cast<unsigned>((ipv4 >> 8u) & 255u),
            static_cast<unsigned>(ipv4 & 255u));
    }
    snapshot.station_ip = station_ip;
    OledDisplay::service(now_ms, snapshot);
}

void note_runtime(bool usb, bool bluetooth, bool wired, bool remote_owned) {
    runtime_bits.store((usb ? 1u : 0u) | (bluetooth ? 2u : 0u) | (wired ? 4u : 0u) |
                       (remote_owned ? 8u : 0u),
                       std::memory_order_relaxed);
}

bool take_save_acknowledged() {
    uint8_t expected = save_completion.load(std::memory_order_acquire);
    if ((expected & 3u) != 3u) return false;
    const bool taken = save_completion.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    if (taken) pending_action.store(SaveAction::None, std::memory_order_release);
    return taken;
}

bool take_save_ready() {
    Completion completion;
    const bool taken = take_completion(completion);
    if (taken) pending_action.store(SaveAction::None, std::memory_order_release);
    return taken;
}

bool take_completion(Completion& completion) {
    const uint8_t value = save_completion.exchange(0, std::memory_order_acq_rel);
    if (!value) return false;
    completion.action = static_cast<SaveAction>(value >> 2);
    completion.peer_acknowledged = (value & 2) != 0;
    // Keep pending_action held until reboot in normal runtime. Tests/legacy
    // consumers may clear it with take_save_ready/acknowledged instead.
    return true;
}

bool install_cancel_requested() {
    return install_cancel_state.load(std::memory_order_acquire) == 1;
}

void acknowledge_install_cancel() {
    uint8_t expected = 1;
    install_cancel_state.compare_exchange_strong(expected, 2, std::memory_order_acq_rel);
}

Health health() {
    const uint8_t value = health_bits.load(std::memory_order_acquire);
    return {bool(value & 1), bool(value & 2), bool(value & 4), bool(value & 8),
            bool(value & 16), bool(value & 32), bool(value & 64)};
}

bool ready() {
    return service_ready.load(std::memory_order_acquire);
}

const char *ssid() {
    return ap_ssid;
}
} // namespace NetworkService

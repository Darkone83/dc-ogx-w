#include "NetworkService/EspAtProtocol.h"
#include "NetworkService/DiscoveryProtocol.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace NetworkService::EspAt {
namespace {
// A normal page can drain first, while perpetually replenished observer traffic
// cannot prevent a requested scan or a due station attempt forever.
constexpr unsigned MAX_CONNECTION_TURNS = 32;
// A CLOSED event can precede the normal command result. Let that result drain
// before synchronizing a missing one, without blocking unrelated sockets for
// the full send/close timeout.
constexpr std::uint32_t CLOSED_RESULT_GRACE_MS = 100;
// Retry optional name service independently of the 60-second IP refresh.
// Back off after an explicit modem failure; never occupy the pipe repeatedly
// while a controller has a lease.
constexpr std::uint32_t MDNS_RETRY_BASE_MS = 5000;
constexpr std::uint32_t MDNS_RETRY_MAX_MS = 60000;
// Give the ESP's own auto-reconnect a short chance to recover before sending
// another potentially 30-second CWJAP over the shared AT command pipe.
constexpr std::uint32_t STATION_AUTO_RECOVERY_MS = 8000;
constexpr std::uint32_t STATION_JOIN_RETRY_MS = 20000;
// ESP8285 NonOS AT 1.6.2.0: fixed-destination broadcast works with CIPMUX=1.
// It does not require CIPDINFO, UDP destination overrides, or a changed Wi-Fi
// mode. Keep all discovery work behind stable station DHCP and ordinary TCP TX.
constexpr unsigned LEGACY_UDP_LINK = 4;
constexpr std::uint32_t LEGACY_UDP_SETTLE_MS = 5000;
constexpr std::uint32_t LEGACY_UDP_BEACON_MS = 5000;

// Avoid AP/STA channel contention once the selected station address is usable.
// Do not toggle on a brief DHCP/reconnect wobble or an active remote lease.
constexpr std::uint32_t AP_CUTOVER_MS = 2500;
constexpr std::uint32_t AP_RECOVERY_MS = 8000;
constexpr std::uint32_t AP_MODE_RETRY_MS = 30000;
bool elapsed(std::uint32_t now, std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

bool equals(const char* text, std::size_t length, const char* expected) {
    return std::strlen(expected) == length && std::memcmp(text, expected, length) == 0;
}

bool number(const char*& p, const char* end, std::uint32_t& value) {
    if (p == end || *p < '0' || *p > '9') return false;
    value = 0;
    do {
        const unsigned digit = static_cast<unsigned>(*p++ - '0');
        if (value > (UINT32_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    } while (p != end && *p >= '0' && *p <= '9');
    return true;
}

bool safe_at_string(const char* value, std::size_t minimum, std::size_t maximum) {
    if (!value) return false;
    std::size_t length = 0;
    for (; value[length] && length <= maximum; ++length) {
        const auto c = static_cast<unsigned char>(value[length]);
        if (c < 32 || c > 126 || c == '"' || c == '\\' || c == ',') return false;
    }
    return length >= minimum && length <= maximum;
}

bool bounded_at_value(const char* value, std::size_t maximum) {
    if (!value) return false;
    for (std::size_t i = 0; i <= maximum; ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (!c) return true;
        if (c < 32 || c == 127) return false;
    }
    return false;
}

void escape_at(const char* input, char* output) {
    // Caller provides 2*maximum+1 bytes. ESP-AT requires escaping commas too,
    // even inside quoted arguments. Never send user-provided CR/LF as commands.
    while (*input) {
        if (*input == '"' || *input == '\\' || *input == ',') *output++ = '\\';
        *output++ = *input++;
    }
    *output = '\0';
}

bool extract_ipv4(const char* text, std::size_t length, char* output) {
    const char* quote = static_cast<const char*>(std::memchr(text, '"', length));
    if (!quote) return false;
    const char* p = quote + 1;
    const char* end = text + length;
    const char* start = p;
    unsigned nonzero = 0;
    for (unsigned part = 0; part != 4; ++part) {
        std::uint32_t value = 0;
        if (!number(p, end, value) || value > 255) return false;
        nonzero |= value;
        if (p == end || *p++ != (part == 3 ? '"' : '.')) return false;
    }
    const auto count = static_cast<std::size_t>(p - start - 1);
    if (!nonzero || count > 15) return false;
    std::memcpy(output, start, count);
    output[count] = '\0';
    return true;
}

bool quoted(const char*& p, const char* end, char* output, std::size_t capacity) {
    if (p == end || *p++ != '"' || capacity == 0) return false;
    std::size_t used = 0;
    while (p != end) {
        unsigned char c = static_cast<unsigned char>(*p++);
        if (c == '"') { output[used] = '\0'; return true; }
        if (c == '\\') {
            if (p == end) return false;
            c = static_cast<unsigned char>(*p++);
            // These are ESP-AT's string escapes, not JSON escape sequences.
            if (c != '"' && c != '\\' && c != ',') return false;
        }
        if (c < 32 || c == 127 || used + 1 >= capacity) return false;
        output[used++] = static_cast<char>(c);
    }
    return false;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool scan_network(const char* text, std::size_t length, WifiScan::Network& network) {
    // NONOS AT includes parentheses; ESP-AT documentation also describes the
    // same five leading fields without them. Extended numeric fields follow.
    if (length < 7 || std::memcmp(text, "+CWLAP:", 7)) return false;
    const char* p = text + 7;
    const char* end = text + length;
    const bool parenthesized = p != end && *p == '(';
    if (parenthesized) ++p;
    std::uint32_t auth, rssi, channel;
    char mac[18]{};
    if (!number(p, end, auth) || p == end || *p++ != ',' ||
        !quoted(p, end, network.ssid, sizeof(network.ssid)) || p == end || *p++ != ',') return false;
    const bool negative = p != end && *p == '-';
    if (negative) ++p;
    if (!number(p, end, rssi) || rssi > 127 || (!negative && rssi != 0) ||
        p == end || *p++ != ',' || !quoted(p, end, mac, sizeof(mac)) ||
        std::strlen(mac) != 17 || p == end || *p++ != ',' ||
        !number(p, end, channel) || channel < 1 || channel > 14) return false;
    for (unsigned i = 0; i < 6; ++i) {
        const int hi = hex_digit(mac[i * 3]), lo = hex_digit(mac[i * 3 + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && mac[i * 3 + 2] != ':')) return false;
        network.bssid[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    // Ignore bounded, numeric extension fields, while rejecting a truncated or
    // malformed record instead of accepting an arbitrary prefix as a network.
    while (p != end && *p == ',') {
        ++p;
        if (p != end && (*p == '-' || *p == '+')) ++p;
        std::uint32_t extra;
        if (!number(p, end, extra)) return false;
    }
    if (parenthesized && (p == end || *p++ != ')')) return false;
    if (p != end) return false;
    network.rssi = static_cast<std::int16_t>(negative ? -static_cast<int>(rssi) : 0);
    network.channel = static_cast<std::uint8_t>(channel);
    using WifiScan::Security;
    switch (auth) {
    case 0: network.security = Security::Open; break;
    case 1: network.security = Security::Wep; break;
    case 2: network.security = Security::Wpa; break;
    case 3: network.security = Security::Wpa2; break;
    case 4: network.security = Security::WpaWpa2; break;
    case 5: network.security = Security::Enterprise; break;
    case 6: network.security = Security::Wpa3; break;
    case 7: network.security = Security::Wpa2Wpa3; break;
    default: network.security = Security::Unknown; break;
    }
    return true;
}
} // namespace

void Parser::reset(const Sink& sink) {
    sink_ = sink;
    used_ = 0;
    remaining_ = 0;
    link_ = 0;
    skip_prompt_space_ = false;
    broken_ = false;
    discarding_scan_line_ = false;
}

void Parser::fail(const char* reason) {
    broken_ = true;
    if (sink_.fault) sink_.fault(sink_.context, reason);
}

bool Parser::ipd_header() {
    const char* p = line_ + 5;
    const char* end = line_ + used_ - 1; // The final colon is not a field.
    std::uint32_t first = 0, length = 0;
    if (!number(p, end, first)) return false;
    if (p == end) {
        link_ = 0;
        length = first; // Single-connection AT firmware, useful during recovery.
    } else {
        if (*p++ != ',' || !number(p, end, length) || first >= 5) return false;
        link_ = static_cast<unsigned>(first);
        // CIPDINFO=1 adds remote IPv4 address and port. Payload length still
        // comes from the first two numeric fields, never from delimiters in data.
        if (p != end && *p != ',') return false;
    }
    if (length > 65535) return false;
    remaining_ = length;
    used_ = 0;
    return true;
}

void Parser::feed(const std::uint8_t* bytes, std::size_t length) {
    while (length && !broken_) {
        if (remaining_) {
            const std::size_t count = std::min<std::size_t>(remaining_, length);
            remaining_ -= static_cast<std::uint32_t>(count);
            if (sink_.payload) sink_.payload(sink_.context, link_, bytes, count);
            bytes += count;
            length -= count;
            continue;
        }
        const char c = static_cast<char>(*bytes++);
        --length;
        if (discarding_scan_line_) {
            if (c == '\n') discarding_scan_line_ = false;
            continue;
        }
        if (skip_prompt_space_) {
            skip_prompt_space_ = false;
            if (c == ' ') continue;
        }
        if (used_ == 0 && c == '>') {
            skip_prompt_space_ = true;
            if (sink_.line) sink_.line(sink_.context, ">", 1);
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            if (used_ >= 5 && std::memcmp(line_, "+IPD,", 5) == 0) {
                fail("esp_passive_ipd"); // Active receive mode is required.
                continue;
            }
            if (used_) {
                line_[used_] = '\0';
                const auto count = used_;
                used_ = 0;
                if (sink_.line) sink_.line(sink_.context, line_, count);
            }
            continue;
        }
        if (used_ == sizeof(line_) - 1) {
            if (used_ >= 7 && std::memcmp(line_, "+CWLAP:", 7) == 0) {
                // A bad/extended scan record must not reset an otherwise
                // healthy AP server. Discard exactly this line, then resume
                // normal framing (including interleaved binary +IPD records).
                used_ = 0;
                discarding_scan_line_ = true;
                constexpr char invalid[] = "+CWLAP:invalid";
                if (sink_.line) sink_.line(sink_.context, invalid, sizeof(invalid) - 1);
                continue;
            }
            fail("esp_line_overflow");
            continue;
        }
        line_[used_++] = c;
        if (c == ':' && used_ >= 6 && std::memcmp(line_, "+IPD,", 5) == 0) {
            if (!ipd_header()) fail("esp_bad_ipd");
        }
    }
}

void Engine::on_line(void* context, const char* text, std::size_t length) {
    static_cast<Engine*>(context)->line(text, length);
}
void Engine::on_payload(void* context, unsigned id, const std::uint8_t* bytes, std::size_t length) {
    static_cast<Engine*>(context)->payload(id, bytes, length);
}
void Engine::on_fault(void* context, const char* reason) {
    auto& engine = *static_cast<Engine*>(context);
    engine.framing_fault_pending_ = true;
    engine.framing_fault_reason_ = reason;
}

void Engine::reset_parser() {
    parser_.reset({this, on_line, on_payload, on_fault});
}

bool Engine::begin(const Transport::Config& config, const Transport::Callbacks& callbacks,
                   const Io& io, std::uint32_t base_baud, std::uint32_t now_ms) {
    if (started_ || !safe_at_string(config.ssid, 1, 32) ||
        !safe_at_string(config.password, 8, 63) || config.channel < 1 || config.channel > 13 ||
        !bounded_at_value(config.station_ssid, 32) || !bounded_at_value(config.station_password, 64) ||
        !safe_at_string(config.hostname, 1, 32) ||
        !callbacks.connected || !callbacks.received || !callbacks.sent || !callbacks.closed ||
        !io.write || !io.tx_idle || !io.set_baud || !io.discard_rx || base_baud < 9600) return false;
    config_ = config;
    callbacks_ = callbacks;
    io_ = io;
    base_baud_ = base_baud;
    baud_ = base_baud;
    now_ = now_ms;
    started_ = true;
    reset_after_probe_ = true;
    error_ = "esp_starting";
    scan_.reset();
    change_baud(base_baud_);
    phase_ = Phase::ProbeQuiet;
    due_ = now_ + 1000; // ESP power-on boot chatter is not an AT response.
    return true;
}

void Engine::change_baud(std::uint32_t baud) {
    io_.set_baud(io_.context, baud);
    baud_ = baud;
    io_.discard_rx(io_.context);
    reset_parser();
}

void Engine::feed(const std::uint8_t* bytes, std::size_t length) {
    if (started_ && !recovery_pending_ && !framing_fault_pending_) {
        if (length) last_rx_ = now_;
        parser_.feed(bytes, length);
    }
}

void Engine::issue(Op op, const char* text, std::uint32_t timeout_ms) {
    const auto length = std::strlen(text);
    if (length >= sizeof(command_)) {
        recover("esp_command_too_long");
        return;
    }
    if (text != command_) std::memcpy(command_, text, length + 1);
    command_length_ = length;
    command_written_ = 0;
    timeout_ms_ = timeout_ms;
    due_ = now_ + timeout_ms;
    saw_no_change_ = false;
    if (op == Op::SendPrompt || op == Op::Close) {
        connection_turns_ = std::min(connection_turns_ + 1, MAX_CONNECTION_TURNS);
        operation_disconnected_ = false;
    } else connection_turns_ = 0;
    op_ = op;
}

void Engine::probe_attempt() {
    const bool fast = probe_attempt_ == 2 || probe_attempt_ == 3 || probe_attempt_ == 5;
    change_baud(fast ? FAST_BAUD : base_baud_);
    if (probe_attempt_ >= 4) {
        phase_ = Phase::EscapeBefore;
        due_ = now_ + 1100; // Hayes escape requires silence on both sides.
    } else {
        phase_ = Phase::ProbeQuiet;
        due_ = now_ + 50;
    }
}

void Engine::probe_failed() {
    op_ = Op::None;
    if (++probe_attempt_ >= 6) {
        phase_ = Phase::RetryWait;
        due_ = now_ + 5000;
        error_ = "esp_not_responding";
    } else probe_attempt();
}

void Engine::start_configuration() {
    op_ = Op::None;
    phase_ = Phase::Configure;
    step_ = Step::Echo;
    variant_ = 0;
    station_ip_[0] = '\0';
    station_linked_ = false;
    station_join_confirmed_ = false;
    station_joining_ = false;
    legacy_udp_ready_ = legacy_udp_disabled_ = legacy_udp_sending_ = false;
    legacy_udp_due_ = now_ + LEGACY_UDP_SETTLE_MS;
    legacy_udp_beacon_due_ = now_;
    station_address_pending_ = false;
    ap_enabled_ = false;
    ap_recovery_pending_ = false;
    ap_mode_variant_ = 0;
    station_error_ = station_configured() ? "station_starting" : "";
    initial_network_pending_ = station_configured();
    mdns_available_ = mdns_pending_ = mdns_stop_pending_ = false;
    mdns_error_ = "";
    mdns_failures_ = 0;
    mdns_due_ = now_;
    reconnect_policy_pending_ = false;
}

bool Engine::station_configured() const { return config_.station_ssid && config_.station_ssid[0]; }

bool Engine::station_operation() const {
    switch (op_) {
    case Op::StationJoin: case Op::StationAddress: case Op::StationMdnsStop:
    case Op::StationMdns: case Op::StationReconnectPolicy: case Op::StationSync:
    case Op::ScanOptions: case Op::Scan: case Op::ScanSync: case Op::ApMode:
        return true;
    default: return false;
    }
}

const char* Engine::mdns_state() const {
    if (!station_configured()) return "not_configured";
    if (!station_linked_ || !station_ip_[0]) return "waiting_for_station";
    if (op_ == Op::StationMdnsStop || op_ == Op::StationMdns) return "starting";
    if (mdns_stop_pending_ || mdns_pending_)
        return control_active_ ? "deferred" : "queued";
    if (mdns_available_) return "active";
    if (!elapsed(now_, mdns_due_)) return "retry_wait";
    return control_active_ ? "deferred" : "queued";
}

void Engine::mdns_failed(const char* reason) {
    mdns_available_ = false;
    mdns_error_ = reason;
    mdns_failures_ = std::min(mdns_failures_ + 1, 5u);
    const auto retry = std::min(MDNS_RETRY_BASE_MS << (mdns_failures_ - 1), MDNS_RETRY_MAX_MS);
    mdns_due_ = now_ + retry;
    initial_network_pending_ = false;
}

bool Engine::retry_connection() {
    if (!started_ || !station_configured()) return false;
    if (station_joining_ || (station_linked_ && station_ip_[0])) return true;
    station_retry_pending_ = true;
    return true;
}

bool Engine::retry_mdns() {
    if (!status().ready || !station_linked_ || !station_ip_[0]) return false;
    if (op_ == Op::StationMdnsStop || op_ == Op::StationMdns || mdns_stop_pending_ || mdns_pending_)
        return true;
    mdns_stop_pending_ = true;
    mdns_due_ = now_;
    return true;
}

bool Engine::request_scan() {
    if (!status().ready) return false;
    if (scan_.queue()) {
        scan_step_ = 0;
        scan_had_rows_ = scan_parsed_rows_ = false;
    }
    return true;
}

const char* Engine::step_error() const {
    switch (step_) {
    case Step::WifiMode: return "esp_ap_mode_failed";
    case Step::AccessPoint: return "esp_ap_config_failed";
    case Step::Ip: return "esp_ap_address_failed";
    case Step::Dhcp: return "esp_dhcp_failed";
    case Step::Server: return "esp_tcp_server_failed";
    default: return "esp_at_config_failed";
    }
}

void Engine::configure() {
    const char* text = nullptr;
    std::uint32_t timeout_ms = 2500;
    switch (step_) {
    case Step::Echo: text = "ATE0\r\n"; break;
    case Step::Version: text = "AT+GMR\r\n"; break;
    case Step::Store: text = "AT+SYSSTORE=0\r\n"; break;
    case Step::Baud:
        if (baud_ == FAST_BAUD || fast_disabled_) {
            configured_step(true);
            return;
        }
        issue(Op::BaudSet, "AT+UART_CUR=460800,8,1,0,0\r\n", 1000);
        return;
    case Step::StopServer: text = "AT+CIPSERVER=0\r\n"; break;
    case Step::CloseAll: text = "AT+CIPCLOSE=5\r\n"; break;
    case Step::Normal: text = "AT+CIPMODE=0\r\n"; break;
    case Step::Mux: text = "AT+CIPMUX=1\r\n"; break;
    case Step::ActiveReceive: text = "AT+CIPRECVMODE=0\r\n"; break;
    case Step::IpInfo: text = "AT+CIPDINFO=0\r\n"; break;
    case Step::WifiMode:
        // Scanning needs the station interface even before a network is saved.
        // Modern mode changes otherwise auto-connect inherited ESP credentials.
        text = variant_ == 0 ? "AT+CWMODE_CUR=3\r\n" :
               variant_ == 1 ? "AT+CWMODE=3,0\r\n" : "AT+CWMODE=3\r\n";
        timeout_ms = 8000;
        break;
    case Step::DisconnectStation:
        // The RP2040 settings own network selection. A modem association from
        // an earlier firmware must not prevent our configured CWJAP attempt.
        text = "AT+CWQAP\r\n";
        break;
    case Step::Hostname:
        if (!station_configured()) { configured_step(true); return; }
        std::snprintf(command_, sizeof(command_), "AT+CWHOSTNAME=\"%s\"\r\n", config_.hostname);
        text = command_;
        break;
    case Step::Ip:
        text = variant_ == 0 ? "AT+CIPAP_CUR=\"192.168.4.1\",\"192.168.4.1\",\"255.255.255.0\"\r\n" :
               variant_ == 1 ? "AT+CIPAP=\"192.168.4.1\",\"192.168.4.1\",\"255.255.255.0\"\r\n" :
                               "AT+CIPAP=\"192.168.4.1\"\r\n";
        break;
    case Step::AccessPoint:
        std::snprintf(command_, sizeof(command_), "AT+CWSAP%s=\"%s\",\"%s\",%u,3\r\n",
                      variant_ == 0 ? "_CUR" : "", config_.ssid, config_.password, config_.channel);
        text = command_;
        timeout_ms = 8000;
        break;
    case Step::Dhcp:
        // Legacy NONOS: <mode>,<enable>. Modern ESP-AT: <enable>,<bitmask>.
        // The modern fallback uses enable=2 in the legacy position and is
        // therefore rejected by legacy firmware instead of disabling its AP.
        text = variant_ == 0 ? "AT+CWDHCP_CUR=2,1\r\n" :
               variant_ == 1 ? "AT+CWDHCP=1,3\r\n" : "AT+CWDHCP=2,1\r\n";
        break;
    case Step::MaxConnections: text = "AT+CIPSERVERMAXCONN=3\r\n"; break;
    case Step::Server: text = "AT+CIPSERVER=1,80\r\n"; break;
    case Step::IdleTimeout: text = "AT+CIPSTO=120\r\n"; break;
    case Step::Done:
        phase_ = Phase::Serving;
        error_ = "";
        ap_enabled_ = true; // Configuration has verified AP mode + server.
        ap_recovery_pending_ = false;
        station_due_ = now_;
        station_error_ = station_configured() ? "station_starting" : "";
        // ESP mDNS is enabled after the station has a known address. Legacy
        // AT firmware may not expose its responder on the SoftAP interface.
        mdns_stop_pending_ = false;
        return;
    }
    issue(Op::Config, text, timeout_ms);
}

void Engine::configured_step(bool success) {
    op_ = Op::None;
    if (!success) {
        unsigned alternatives = 0;
        switch (step_) {
        case Step::AccessPoint: alternatives = 1; break;
        case Step::WifiMode: case Step::Ip: case Step::Dhcp: alternatives = 2; break;
        default: break;
        }
        if (variant_ < alternatives) {
            ++variant_;
            return;
        }
        const bool optional = step_ == Step::Version || step_ == Step::Store ||
            step_ == Step::StopServer || step_ == Step::CloseAll ||
            step_ == Step::ActiveReceive || step_ == Step::IpInfo ||
            step_ == Step::MaxConnections || step_ == Step::IdleTimeout || step_ == Step::Hostname ||
            step_ == Step::DisconnectStation;
        if (!optional) {
            recover(step_error());
            return;
        }
    }
    variant_ = 0;
    step_ = static_cast<Step>(static_cast<unsigned>(step_) + 1);
}

void Engine::start_baud_verify(bool fast) {
    op_ = Op::None;
    verifying_fast_ = fast;
    phase_ = Phase::BaudQuiet;
    due_ = now_ + 50;
    // The command may reply at its old or its new baud. Always verify explicitly.
    change_baud(fast ? FAST_BAUD : base_baud_);
}

void Engine::terminal(bool success) {
    switch (op_) {
    case Op::Probe:
        // The leading CRLF deliberately finishes a possibly truncated command.
        // Its ERROR may precede the actual AT OK; let the probe deadline decide.
        if (!success) break;
        op_ = Op::None;
        probe_attempt_ = 0;
        if (reset_after_probe_) {
            reset_after_probe_ = false;
            issue(Op::Reset, "AT+RST\r\n", 2000);
        } else start_configuration();
        break;
    case Op::Reset:
        op_ = Op::None;
        if (success) {
            phase_ = Phase::ResetWait;
            due_ = now_ + 1500;
        } else start_configuration();
        break;
    case Op::Config: configured_step(success); break;
    case Op::BaudSet:
        if (success) start_baud_verify(true);
        else {
            fast_disabled_ = true;
            configured_step(true);
        }
        break;
    case Op::BaudVerify:
        if (!success) break; // Same CRLF/ERROR synchronization case as Probe.
        op_ = Op::None;
        if (!verifying_fast_) fast_disabled_ = true;
        phase_ = Phase::Configure;
        configured_step(true);
        break;
    case Op::Close: {
        const auto id = operation_link_;
        const auto handle = operation_handle_;
        op_ = Op::None;
        operation_handle_ = 0;
        if (id < LINK_COUNT && links_[id].handle == handle) {
            notify_closed(links_[id]);
            clear_link(links_[id]);
        }
        break;
    }
    case Op::SendPrompt:
        if (!success) complete_send(false);
        break; // Some AT versions send OK before the data prompt.
    case Op::SendResult:
        if (!success) complete_send(false);
        break;
    case Op::StationJoin: case Op::StationAddress: case Op::StationMdnsStop:
    case Op::StationMdns: case Op::StationReconnectPolicy:
        station_terminal(success);
        break;
    case Op::LegacyUdpOpen:
        op_ = Op::None;
        if (success) {
            legacy_udp_ready_ = true;
            legacy_udp_beacon_due_ = now_ + 1000;
        } else {
            // Optional feature unsupported or link 4 unavailable: do not
            // reset a working HTTP server or keep retrying at runtime.
            legacy_udp_disabled_ = true;
        }
        break;
    case Op::ApMode:
        op_ = Op::None;
        if (success) {
            ap_enabled_ = ap_mode_target_enabled_;
            ap_recovery_pending_ = false;
            ap_mode_variant_ = 0;
            // The association can change while a mode command is in flight.
            // Schedule the opposite transition if its result is now stale.
            if (!ap_enabled_ && (!station_linked_ || !station_ip_[0]))
                request_ap_recovery();
            else if (ap_enabled_ && station_linked_ && station_ip_[0])
                ap_mode_due_ = now_ + AP_CUTOVER_MS;
        } else if (++ap_mode_variant_ >= 3) {
            // Keep advertising the last confirmed state. A failed mode switch
            // must never falsely hide the recovery address from the user.
            ap_mode_variant_ = 0;
            ap_mode_due_ = now_ + AP_MODE_RETRY_MS;
        }
        break;
    case Op::StationSync:
    case Op::ScanSync:
    case Op::ConnectionSync:
        if (success && sync_version_seen_) op_ = Op::None;
        break;
    case Op::ScanOptions:
        op_ = Op::None;
        scan_step_ = !success && scan_step_ == 0 ? 1 : 2;
        break;
    case Op::Scan:
        op_ = Op::None;
        if (success && (!scan_had_rows_ || scan_parsed_rows_)) scan_.complete(now_);
        else scan_.fail(success ? "scan_response_invalid" : "scan_failed", now_);
        break;
    default: break;
    }
}

void Engine::timeout() {
    if (phase_ == Phase::Serving && command_timeouts_ != UINT32_MAX) ++command_timeouts_;
    switch (op_) {
    case Op::Probe: probe_failed(); break;
    case Op::Reset:
        op_ = Op::None;
        phase_ = Phase::ResetWait;
        due_ = now_ + 1000;
        break;
    case Op::BaudSet: start_baud_verify(true); break;
    case Op::BaudVerify:
        if (verifying_fast_) start_baud_verify(false);
        else recover("esp_baud_unconfirmed");
        break;
    case Op::SendPrompt:
        // A lost prompt can leave the modem expecting exactly send_length_
        // bytes. Finish that bounded record before attempting AT recovery.
        abandon_send_ = true;
        op_ = Op::SendBody;
        send_written_ = 0;
        due_ = now_ + 3000;
        error_ = "esp_send_prompt_timeout";
        break;
    case Op::SendBody: recover("esp_uart_tx_timeout"); break;
    case Op::SendResult:
        if (legacy_udp_sending_) {
            // An ambiguous optional datagram must never trigger a modem reset
            // or be replayed. Identify the command pipe before other sends.
            legacy_udp_sending_ = false;
            legacy_udp_disabled_ = true;
            legacy_udp_ready_ = false;
            op_ = Op::None;
            operation_handle_ = 0;
            synchronize(Op::ConnectionSync);
            break;
        }
        if (!abandon_send_) {
            // A confirmed prompt and the entire promised UART body have both
            // completed. A missing SEND terminal is now an ambiguous delivery
            // for this connection, not proof that every socket is broken.
            // Fail this generation once, then identify the AT command pipe
            // with GMR before closing it or sending unrelated queued output.
            // Never replay the possibly delivered body.
            complete_send(false);
            error_ = "esp_send_timeout";
            synchronize(Op::ConnectionSync);
        } else recover("esp_send_timeout");
        break;
    case Op::Config:
        if (saw_no_change_) configured_step(true);
        else recover(step_error());
        break;
    case Op::Close:
        if (operation_disconnected_) {
            op_ = Op::None;
            operation_handle_ = 0;
            synchronize(Op::ConnectionSync);
        } else recover("esp_close_timeout");
        break;
    case Op::Escape: probe_failed(); break;
    case Op::StationJoin:
        request_ap_recovery();
        station_error_ = "station_join_timeout";
        station_join_confirmed_ = false;
        station_address_pending_ = false;
        station_address_seen_ = false;
        station_joining_ = false;
        station_due_ = now_ + STATION_JOIN_RETRY_MS;
        station_linked_ = false;
        station_ip_[0] = '\0';
        initial_network_pending_ = false;
        // A late CWJAP OK must not become the result of a CIPSEND/MDNS command.
        // Synchronize with AT before using the pipe again; no blind join retry.
        synchronize(Op::StationSync);
        break;
    case Op::StationSync: recover("esp_station_command_unresponsive"); break;
    case Op::LegacyUdpOpen:
        // The modem may have opened link 4 without its terminal OK. No retry
        // until a reboot; synchronize its AT pipe before using TCP again.
        legacy_udp_disabled_ = true;
        legacy_udp_ready_ = false;
        synchronize(Op::ConnectionSync);
        break;
    case Op::ApMode:
        // Mode changes may take effect even if the terminal response is lost.
        // Re-probe/reconfigure rather than guessing whether the AP is up.
        recover("esp_ap_mode_unconfirmed");
        break;
    case Op::ScanSync: recover("esp_scan_command_unresponsive"); break;
    case Op::ConnectionSync: recover("esp_connection_command_unresponsive"); break;
    case Op::ScanOptions: case Op::Scan:
        scan_.fail(op_ == Op::Scan ? "scan_timeout" : "scan_setup_timeout", now_);
        synchronize(Op::ScanSync);
        break;
    case Op::StationAddress: case Op::StationMdnsStop: case Op::StationMdns:
    case Op::StationReconnectPolicy:
        station_terminal(false, true);
        synchronize(Op::StationSync);
        break;
    default: break;
    }
}

void Engine::line(const char* text, std::size_t length) {
    if (equals(text, length, "ready")) {
        close_all();
        op_ = Op::None;
        operation_handle_ = 0;
        reset_after_probe_ = false;
        recovery_pending_ = false;
        phase_ = Phase::ResetWait;
        due_ = now_ + 500;
        error_ = "esp_restarting";
        station_ip_[0] = '\0';
        station_linked_ = false;
        station_join_confirmed_ = false;
        station_joining_ = false;
        ap_enabled_ = false;
        ap_recovery_pending_ = false;
        mdns_available_ = mdns_pending_ = mdns_stop_pending_ = false;
        initial_network_pending_ = station_configured();
        if (scan_.status().phase == WifiScan::Phase::Queued || scan_.status().phase == WifiScan::Phase::Running)
            scan_.fail("scan_radio_restarted", now_);
        return;
    }
    if (equals(text, length, "WIFI DISCONNECT")) {
        legacy_udp_ready_ = false;
        legacy_udp_sending_ = false;
        legacy_udp_due_ = now_ + LEGACY_UDP_SETTLE_MS;
        request_ap_recovery();
        station_linked_ = false;
        station_ip_[0] = '\0';
        station_address_pending_ = false;
        // CWJAP may first disconnect an old association. Its final result, not
        // that intermediate event, decides whether the new attempt is complete.
        if (op_ != Op::StationJoin) station_joining_ = false;
        mdns_available_ = mdns_pending_ = mdns_stop_pending_ = false;
        if (phase_ == Phase::Serving && op_ != Op::StationJoin)
            initial_network_pending_ = false;
        station_error_ = station_configured() ? "station_disconnected" : "";
        station_due_ = now_ + STATION_AUTO_RECOVERY_MS;
        return;
    }
    if (equals(text, length, "WIFI CONNECTED")) {
        if (station_configured() && station_join_confirmed_) {
            station_linked_ = true;
            // Re-association can happen without a fresh CWJAP response. Query
            // the lease once the serialized AT pipe is free, rather than
            // waiting for a 60s periodic status cycle.
            station_address_pending_ = true;
            station_address_variant_ = 0;
            address_due_ = now_;
        }
        return;
    }
    if (equals(text, length, "WIFI GOT IP")) {
        if (!station_configured() || !station_join_confirmed_) return;
        ap_recovery_pending_ = false;
        station_linked_ = true;
        station_joining_ = !station_ip_[0];
        station_address_pending_ = true;
        station_address_variant_ = 0;
        station_due_ = now_ + 60000;
        return;
    }
    if (length >= 11 && std::memcmp(text, "AT version:", 11) == 0) {
        const auto count = std::min(length, sizeof(modem_firmware_) - 1);
        for (std::size_t i = 0; i != count; ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            modem_firmware_[i] = c >= 32 && c <= 126 && c != '"' && c != '\\' ? static_cast<char>(c) : '?';
        }
        modem_firmware_[count] = '\0';
        if (op_ == Op::StationSync || op_ == Op::ScanSync || op_ == Op::ConnectionSync)
            sync_version_seen_ = true;
        return;
    }
    if (op_ == Op::Scan && length >= 7 && std::memcmp(text, "+CWLAP:", 7) == 0) {
        scan_had_rows_ = true;
        WifiScan::Network network{};
        if (scan_network(text, length, network)) {
            scan_parsed_rows_ = true;
            scan_.add(network);
        }
        return;
    }
    if (op_ == Op::StationJoin && length >= 7 && std::memcmp(text, "+CWJAP", 6) == 0) {
        const auto* colon = static_cast<const char*>(std::memchr(text, ':', length));
        if (colon && colon + 1 < text + length) {
            station_join_reported_error_ = true;
            station_error_ = colon[1] == '1' ? "station_join_timeout" :
                             colon[1] == '2' ? "station_bad_password" :
                             colon[1] == '3' ? "station_not_found" : "station_join_failed";
        }
        return;
    }
    if (op_ == Op::StationAddress &&
        ((length >= 11 && std::memcmp(text, "+CIPSTA:ip:", 11) == 0) ||
         (length >= 15 && std::memcmp(text, "+CIPSTA_CUR:ip:", 15) == 0) ||
         (length >= 13 && std::memcmp(text, "+CIFSR:STAIP,", 13) == 0))) {
        station_address_seen_ = extract_ipv4(text, length, station_ip_);
        return;
    }
    if (op_ == Op::LegacyUdpOpen && equals(text, length, "ALREADY CONNECT")) {
        terminal(true);
        return;
    }
    if (length >= 3 && text[0] >= '0' && text[0] <= '4' && text[1] == ',') {
        const auto id = static_cast<unsigned>(text[0] - '0');
        if (id == LEGACY_UDP_LINK && (op_ == Op::LegacyUdpOpen || legacy_udp_ready_)) {
            if (equals(text + 2, length - 2, "CLOSED") ||
                equals(text + 2, length - 2, "CONNECT FAIL")) {
                legacy_udp_ready_ = false;
                legacy_udp_disabled_ = true; // Optional socket; TCP keeps serving.
            }
            return; // UDP must never allocate an HTTP client slot.
        }
        if (equals(text + 2, length - 2, "CONNECT") || equals(text + 2, length - 2, "OPEN")) {
            connected(id);
            return;
        }
        if (equals(text + 2, length - 2, "CLOSED") || equals(text + 2, length - 2, "CONNECT FAIL")) {
            disconnected(id);
            return;
        }
    }
    if (op_ == Op::None || command_written_ != command_length_) return;
    if ((op_ == Op::StationSync || op_ == Op::ScanSync || op_ == Op::ConnectionSync) && !sync_version_seen_) {
        if (length >= 6 && std::memcmp(text, "busy p", 6) == 0) {
            sync_busy_ = true;
            return;
        }
        if (sync_busy_ && !sync_retried_ &&
            (equals(text, length, "SEND OK") || equals(text, length, "SEND FAIL") ||
             equals(text, length, "OK") || equals(text, length, "ERROR") ||
             equals(text, length, "FAIL") || equals(text, length, "link is not valid"))) {
            // A timeout/CLOSED does not terminate the outstanding AT command.
            // If GMR was rejected as busy, consume that command's late result
            // and retry synchronization once. Never advance from a bare OK.
            sync_busy_ = false;
            sync_retried_ = true;
            issue(op_, "\r\nAT+GMR\r\n", 5000);
            return;
        }
    }
    if (equals(text, length, ">") && op_ == Op::SendPrompt) {
        op_ = Op::SendBody;
        send_written_ = 0;
        due_ = now_ + 3000;
    } else if (equals(text, length, "SEND OK") && op_ == Op::SendResult) {
        complete_send(true);
    } else if (equals(text, length, "SEND FAIL") &&
               (op_ == Op::SendPrompt || op_ == Op::SendResult)) {
        complete_send(false);
    } else if (equals(text, length, "OK") || equals(text, length, "no change")) {
        // 'no change' on old CWMODE firmware can be followed by OK. Waiting for
        // the final OK avoids crediting it to the next command.
        if (equals(text, length, "OK")) terminal(true);
        else if (op_ == Op::Config && step_ == Step::WifiMode) saw_no_change_ = true;
    } else if (equals(text, length, "ERROR") || equals(text, length, "FAIL") ||
               equals(text, length, "link is not valid")) {
        terminal(false);
    }
    // busy p/s and informational/GMR lines are
    // unsolicited; only a terminal response or a deadline advances a command.
}

void Engine::payload(unsigned id, const std::uint8_t* bytes, std::size_t length) {
    if (id == LEGACY_UDP_LINK && legacy_udp_ready_) return; // Broadcast-only.
    if (id >= LINK_COUNT || phase_ != Phase::Serving || recovery_pending_ || abandon_send_) return;
    const auto& link = links_[id];
    if (link.occupied && link.accepted && !link.closing)
        callbacks_.received(link.handle, bytes, length);
}

void Engine::notify_closed(Link& link) {
    if (!link.accepted) return;
    const auto handle = link.handle;
    link.accepted = false; // Reentrant close/write cannot notify or use it twice.
    callbacks_.closed(handle);
}

void Engine::clear_link(Link& link) {
    link.occupied = false;
    link.accepted = false;
    link.closing = false;
    link.abort = false;
    link.queued = 0;
    link.handle = 0;
}

void Engine::close_all() {
    for (auto& link : links_) {
        notify_closed(link);
        clear_link(link);
    }
}

void Engine::connected(unsigned id) {
    auto& link = links_[id];
    // Repeated CONNECT is not a new session unless the preceding connection
    // was closed. Do not replace a live client's buffers on a duplicate notice.
    if (link.occupied && !link.closing) return;
    notify_closed(link);
    clear_link(link);
    link.occupied = true;
    link.handle = next_handle_++;
    if (next_handle_ == 0) ++next_handle_;
    unsigned accepted = 0;
    for (const auto& candidate : links_) if (candidate.accepted) ++accepted;
    // Do not let an old CIPSEND/CIPCLOSE completion target a newly reused ID.
    const bool old_operation = operation_handle_ && operation_link_ == id && op_ != Op::None;
    if (phase_ == Phase::Serving && !recovery_pending_ && !old_operation &&
        accepted < Transport::MAX_CONNECTIONS) {
        link.accepted = callbacks_.connected(link.handle);
    }
    if (!link.accepted) link.closing = link.abort = true;
}

void Engine::disconnected(unsigned id) {
    auto& link = links_[id];
    if (operation_handle_ && operation_link_ == id && operation_handle_ == link.handle) {
        operation_disconnected_ = true;
        if (op_ == Op::SendResult || op_ == Op::Close)
            due_ = now_ + CLOSED_RESULT_GRACE_MS;
    }
    notify_closed(link);
    clear_link(link);
}

Engine::Link* Engine::find(Transport::Handle handle) {
    if (!handle) return nullptr;
    for (auto& link : links_)
        if (link.occupied && link.accepted && link.handle == handle) return &link;
    return nullptr;
}

std::size_t Engine::write(Transport::Handle handle, const std::uint8_t* bytes, std::size_t length) {
    auto* link = find(handle);
    if (!link || link->closing || phase_ != Phase::Serving || recovery_pending_ ||
        abandon_send_ || framing_fault_pending_ || !bytes) return 0;
    const auto count = std::min({length, TX_CAPACITY - link->queued, Transport::MAX_WRITE_SIZE});
    if (count) std::memcpy(link->tx + link->queued, bytes, count);
    link->queued += count;
    return count;
}

void Engine::close(Transport::Handle handle, bool abort) {
    auto* link = find(handle);
    if (!link) return;
    link->closing = true;
    link->abort = link->abort || abort;
    if (link->abort) {
        link->queued = 0;
        notify_closed(*link);
    }
}

void Engine::complete_send(bool success) {
    if (legacy_udp_sending_) {
        const bool ambiguous = abandon_send_;
        legacy_udp_sending_ = false;
        abandon_send_ = false;
        send_length_ = send_written_ = 0;
        op_ = Op::None;
        operation_handle_ = 0;
        if (!success || ambiguous) {
            legacy_udp_disabled_ = true;
            legacy_udp_ready_ = false;
            if (ambiguous) synchronize(Op::ConnectionSync);
        } else {
            if (legacy_udp_beacons_ != UINT32_MAX) ++legacy_udp_beacons_;
            legacy_udp_beacon_due_ = now_ + LEGACY_UDP_BEACON_MS;
        }
        return;
    }
    const auto handle = operation_handle_;
    const auto id = operation_link_;
    const auto count = send_length_;
    op_ = Op::None;
    operation_handle_ = 0;
    operation_disconnected_ = false;
    send_length_ = send_written_ = 0;
    if (abandon_send_) {
        abandon_send_ = false;
        recover("esp_send_resynchronized");
        return;
    }
    if (id >= LINK_COUNT || links_[id].handle != handle) return;
    auto& link = links_[id];
    if (!success) {
        link.closing = link.abort = true;
        link.queued = 0;
        error_ = "esp_send_failed";
        notify_closed(link);
        return;
    }
    if (link.abort || !link.accepted || link.queued < count) return;
    link.queued -= count;
    std::memmove(link.tx, link.tx + count, link.queued);
    callbacks_.sent(handle, count, Transport::Delivery::ModuleAccepted);
}

void Engine::service_connections() {
    for (unsigned id = 0; id != LINK_COUNT; ++id) {
        auto& link = links_[id];
        if (link.occupied && link.closing && (link.abort || link.queued == 0)) {
            notify_closed(link);
            operation_link_ = id;
            operation_handle_ = link.handle;
            std::snprintf(command_, sizeof(command_), "AT+CIPCLOSE=%u\r\n", id);
            issue(Op::Close, command_, 2000);
            return;
        }
    }
    for (unsigned n = 0; n != LINK_COUNT; ++n) {
        const unsigned id = (next_link_ + n) % LINK_COUNT;
        auto& link = links_[id];
        if (!link.occupied || !link.accepted || link.abort || link.queued == 0) continue;
        send_length_ = std::min<std::size_t>(link.queued, SEND_CAPACITY);
        std::memcpy(sending_, link.tx, send_length_);
        send_written_ = 0;
        operation_link_ = id;
        operation_handle_ = link.handle;
        abandon_send_ = false;
        next_link_ = (id + 1) % LINK_COUNT;
        std::snprintf(command_, sizeof(command_), "AT+CIPSEND=%u,%u\r\n", id,
                      static_cast<unsigned>(send_length_));
        issue(Op::SendPrompt, command_, 2500);
        return;
    }
}

void Engine::station_terminal(bool success, bool timed_out) {
    const auto operation = op_;
    op_ = Op::None;
    switch (operation) {
    case Op::StationJoin:
        if (!success) {
            request_ap_recovery();
            station_join_confirmed_ = false;
            station_address_pending_ = false;
            station_address_seen_ = false;
        }
        if (!success && !station_join_plain_ && !station_join_reported_error_) {
            // Modern ESP-AT removed _CUR; only a command-level rejection, not
            // an authentication failure, tries the plain dialect immediately.
            station_join_plain_ = true;
            station_due_ = now_;
            station_linked_ = false;
            break;
        }
        // Respect bad credentials; otherwise retry transient join failures
        // sooner than the old unconditional 60-second delay.
        station_due_ = now_ + (success ? 60000 :
            (std::strcmp(station_error_, "station_bad_password") == 0 ? 60000 : STATION_JOIN_RETRY_MS));
        if (success) {
            // Only a successful CWJAP for our configured SSID establishes
            // association ownership. Pre-OK/failed-attempt IP events cannot
            // bless an inherited modem connection as the selected network.
            station_join_confirmed_ = true;
            station_linked_ = true;
            station_error_ = "station_waiting_for_address";
            station_address_pending_ = true;
            station_address_variant_ = 0;
            // Keep supported firmware from background-roaming during active
            // SoftAP control. The older AT 1.x branch rejects this command;
            // skip that extra UART transaction on the installed ESP8285.
            reconnect_policy_pending_ =
                std::strncmp(modem_firmware_, "AT version:1.", 13) != 0;
        } else {
            station_linked_ = false;
            station_joining_ = false;
            initial_network_pending_ = false;
            station_ip_[0] = '\0';
            if (!station_join_reported_error_) station_error_ = "station_join_failed";
        }
        break;
    case Op::StationAddress:
        if ((!success || !station_address_seen_) && station_address_variant_ < 2) {
            ++station_address_variant_;
            station_address_pending_ = true;
            break;
        }
        station_address_pending_ = false;
        station_joining_ = false;
        // Some older modules acknowledge the association before their DHCP
        // address is usable. Retry confirmation promptly, without rejoining.
        address_due_ = now_ + (success && station_address_seen_ ? 60000 : 5000);
        if (success && station_address_seen_) {
            station_linked_ = true;
            // Only now do we begin the station-settle clock for UDP. No AT
            // discovery commands may run during boot, CWJAP or DHCP.
            if (!legacy_udp_ready_ && !legacy_udp_disabled_)
                legacy_udp_due_ = now_ + LEGACY_UDP_SETTLE_MS;
            ap_recovery_pending_ = false;
            if (ap_enabled_) ap_mode_due_ = now_ + AP_CUTOVER_MS;
            station_retry_pending_ = false;
            station_error_ = "";
            if (mdns_available_ && std::strcmp(mdns_ip_, station_ip_) != 0) {
                // A legacy responder may retain the old IPv4 address across
                // DHCP changes. Reinitialize once on a confirmed change.
                mdns_available_ = false;
                mdns_error_ = "";
                mdns_failures_ = 0;
            }
            if (!mdns_available_ && !mdns_error_[0]) {
                mdns_stop_pending_ = true;
                mdns_due_ = now_;
            }
        } else {
            request_ap_recovery();
            station_ip_[0] = '\0';
            station_error_ = "station_address_unavailable";
            mdns_available_ = false;
            initial_network_pending_ = false;
        }
        break;
    case Op::StationMdnsStop:
        if (timed_out) mdns_error_ = "mdns_stop_timeout";
        mdns_pending_ = station_linked_ && station_ip_[0];
        if (!mdns_pending_) initial_network_pending_ = false;
        break;
    case Op::StationMdns:
        if (success && station_linked_ && station_ip_[0]) {
            mdns_available_ = true;
            mdns_error_ = "";
            mdns_failures_ = 0;
            std::memcpy(mdns_ip_, station_ip_, sizeof(mdns_ip_));
            initial_network_pending_ = false;
        } else mdns_failed(success ? "mdns_station_disconnected" :
                           timed_out ? "mdns_command_timeout" : "mdns_command_rejected");
        break;
    default: break;
    }
}

void Engine::request_ap_recovery() {
    if (!ap_enabled_ && !ap_recovery_pending_) {
        ap_recovery_pending_ = true;
        ap_mode_due_ = now_ + AP_RECOVERY_MS;
    }
}

void Engine::service_ap_mode() {
    if (op_ != Op::None || control_active_ || !station_configured()) return;
    const bool station_has_ip = station_linked_ && station_ip_[0];
    const bool enable_ap = !station_has_ip;
    if (enable_ap == ap_enabled_ || !elapsed(now_, ap_mode_due_)) return;
    if (enable_ap && !ap_recovery_pending_) return;
    // _CUR is supported by the installed legacy ESP8285 modem; modern
    // ESP-AT uses CWMODE=<mode>,0. Keep flash untouched and disable any
    // inherited auto-join behavior on modern modems.
    ap_mode_target_enabled_ = enable_ap;
    const unsigned mode = enable_ap ? 3 : 1;
    std::snprintf(command_, sizeof(command_), ap_mode_variant_ == 0
        ? "AT+CWMODE_CUR=%u\r\n" : ap_mode_variant_ == 1
        ? "AT+CWMODE=%u,0\r\n" : "AT+CWMODE=%u\r\n", mode);
    issue(Op::ApMode, command_, 8000);
}

void Engine::service_station() {
    // Every station command shares the same serialized AT pipe as controller
    // acknowledgements and HTTP replies. Even a periodic IP/mDNS query can
    // stall those sends for its full timeout on older modem firmware. Keep
    // pending metadata work until control/maintenance releases the pipe.
    // control_ready prevents a new lease from starving the first setup pass;
    // an operation already in flight still runs to its terminal response.
    if (op_ != Op::None || control_active_) return;
    if (reconnect_policy_pending_) {
        reconnect_policy_pending_ = false;
        // ESP8266 AP+STA uses one RF channel. Do not let a newer modem
        // continuously scan/reconnect while a controller owns the SoftAP.
        // Host-managed retries below still run when control becomes idle.
        issue(Op::StationReconnectPolicy, "AT+CWRECONNCFG=0,0\r\n", 1500);
        return;
    }
    if (station_address_pending_ ||
        (station_configured() && station_linked_ && elapsed(now_, address_due_))) {
        station_address_pending_ = false;
        station_address_seen_ = false;
        issue(Op::StationAddress, station_address_variant_ == 0 ? "AT+CIPSTA_CUR?\r\n" :
            station_address_variant_ == 1 ? "AT+CIPSTA?\r\n" : "AT+CIFSR\r\n", 1500);
        return;
    }
    if (!mdns_available_ && station_linked_ && station_ip_[0] &&
        !mdns_stop_pending_ && !mdns_pending_ && elapsed(now_, mdns_due_))
        mdns_stop_pending_ = true;
    if (mdns_stop_pending_) {
        mdns_stop_pending_ = false;
        mdns_available_ = false;
        issue(Op::StationMdnsStop, "AT+MDNS=0\r\n", 1500);
        return;
    }
    if (mdns_pending_) {
        mdns_pending_ = false;
        if (mdns_attempts_ != UINT32_MAX) ++mdns_attempts_;
        std::snprintf(command_, sizeof(command_), "AT+MDNS=1,\"%s\",\"_http\",80\r\n", config_.hostname);
        issue(Op::StationMdns, command_, 1500);
        return;
    }
    if (!station_configured() ||
        (!station_retry_pending_ && (station_linked_ || !elapsed(now_, station_due_)))) return;
    // CWJAP can occupy the AT command pipe for many seconds. Protect actual
    // remote ownership, without treating an idle dashboard as a controller.
    // Connection output was serviced first, so a queued Retry response can
    // leave the UART before this longer command takes the pipe.
    char escaped_ssid[65], escaped_password[129];
    escape_at(config_.station_ssid, escaped_ssid);
    escape_at(config_.station_password, escaped_password);
    std::snprintf(command_, sizeof(command_), "AT+CWJAP%s=\"%s\",\"%s\"\r\n",
                  station_join_plain_ ? "" : "_CUR", escaped_ssid, escaped_password);
    station_join_reported_error_ = false;
    station_join_confirmed_ = false;
    station_retry_pending_ = false;
    station_linked_ = false;
    station_address_pending_ = false;
    station_address_seen_ = false;
    initial_network_pending_ = true;
    mdns_available_ = mdns_pending_ = mdns_stop_pending_ = false;
    mdns_error_ = "";
    mdns_failures_ = 0;
    mdns_due_ = now_;
    station_ip_[0] = '\0';
    station_joining_ = true;
    station_error_ = "station_joining";
    if (station_reconnects_ != UINT32_MAX) ++station_reconnects_;
    issue(Op::StationJoin, command_, 30000);
}

void Engine::service_scan() {
    if (op_ != Op::None || control_active_ || scan_.status().phase != WifiScan::Phase::Queued) return;
    if (scan_step_ < 2) {
        // The extended form clears any pre-existing filter; NONOS AT normally
        // supports only the two-field form. Both are optional capabilities.
        issue(Op::ScanOptions, scan_step_ == 0 ? "AT+CWLAPOPT=1,31,-100,65535\r\n" :
                                               "AT+CWLAPOPT=1,31\r\n", 1500);
        return;
    }
    scan_.start();
    issue(Op::Scan, "AT+CWLAP\r\n", 15000);
}

void Engine::service_legacy_udp() {
    if (op_ != Op::None || phase_ != Phase::Serving || legacy_udp_disabled_ ||
        !station_linked_ || !station_ip_[0] || station_joining_ ||
        !elapsed(now_, legacy_udp_due_)) return;
    if (!legacy_udp_ready_) {
        // Legacy ESP8266 NonOS AT 1.6.x syntax; no newer CIPDINFO command.
        issue(Op::LegacyUdpOpen,
            "AT+CIPSTART=4,\"UDP\",\"255.255.255.255\",38473,38473,0\r\n", 4000);
        return;
    }
    if (!elapsed(now_, legacy_udp_beacon_due_)) return;
    // Low-priority, tiny periodic packet, sent only when TCP output is idle.
    // The companion listens on 38473; no direct ephemeral-port replies needed.
    const std::size_t length = DiscoveryProtocol::make_announcement(
        reinterpret_cast<char*>(sending_), sizeof(sending_),
        config_.device_id && config_.device_id[0] ? config_.device_id : config_.ssid,
        "pico_w_2023", station_ip_);
    if (!length) {
        legacy_udp_disabled_ = true;
        return;
    }
    send_length_ = length;
    send_written_ = 0;
    legacy_udp_sending_ = true;
    abandon_send_ = false;
    operation_link_ = LEGACY_UDP_LINK;
    operation_handle_ = 0;
    std::snprintf(command_, sizeof(command_), "AT+CIPSEND=4,%u\r\n",
                  static_cast<unsigned>(send_length_));
    issue(Op::SendPrompt, command_, 2500);
}

void Engine::service_network_jobs() {
    // Complete an in-flight join/address/name-service handshake before a scan.
    // A queued scan can still precede a not-yet-started automatic join.
    if (station_joining_ || mdns_stop_pending_ || mdns_pending_)
        service_station();
    // Recovery/STA-only handoff outranks a newly requested scan so the AP
    // does not stay enabled just because the UI is polling for networks.
    // Legacy modem stays in boot-time AP+STA mode. Switching CWMODE while
    // TCP sockets exist causes network outages on this installed firmware.
    // Do not run service_ap_mode() here.
    if (op_ == Op::None) service_scan();
    if (op_ == Op::None && scan_.status().phase != WifiScan::Phase::Queued) service_station();
}

void Engine::synchronize(Op operation) {
    // An untagged AT/OK exchange can mistake a late CWJAP/CWLAP completion for
    // its own response. GMR supplies an identifiable body before its final OK.
    sync_version_seen_ = false;
    sync_busy_ = sync_retried_ = false;
    issue(operation, "\r\nAT+GMR\r\n", 5000);
}

void Engine::recover(const char* reason, bool needs_reset) {
    error_ = reason;
    recovery_pending_ = true;
    recovery_needs_reset_ = needs_reset;
}

void Engine::perform_recovery() {
    if (recoveries_ != UINT32_MAX) ++recoveries_;
    close_all();
    recovery_pending_ = false;
    op_ = Op::None;
    operation_handle_ = 0;
    send_length_ = send_written_ = 0;
    abandon_send_ = false;
    reset_after_probe_ = recovery_needs_reset_;
    probe_attempt_ = 0;
    phase_ = Phase::RetryWait;
    due_ = now_ + 1000;
    io_.discard_rx(io_.context);
    reset_parser();
    station_ip_[0] = '\0';
    station_linked_ = false;
    station_join_confirmed_ = false;
    station_joining_ = false;
    ap_enabled_ = false;
    ap_recovery_pending_ = false;
    legacy_udp_ready_ = legacy_udp_sending_ = false;
    legacy_udp_disabled_ = false;
    legacy_udp_due_ = now_ + LEGACY_UDP_SETTLE_MS;
    mdns_available_ = mdns_pending_ = mdns_stop_pending_ = false;
    initial_network_pending_ = station_configured();
    if (scan_.status().phase == WifiScan::Phase::Queued || scan_.status().phase == WifiScan::Phase::Running)
        scan_.fail("scan_radio_unavailable", now_);
}

void Engine::receive_overrun() {
    if (started_) {
        framing_fault_pending_ = true;
        framing_fault_reason_ = "esp_uart_rx_overrun";
    }
}

void Engine::poll(std::uint32_t now_ms) {
    if (!started_) return;
    now_ = now_ms;
    if (parser_.awaiting_payload() && elapsed(now_, last_rx_ + 2000)) {
        framing_fault_pending_ = true;
        framing_fault_reason_ = "esp_ipd_timeout";
    }
    if (framing_fault_pending_) {
        framing_fault_pending_ = false;
        // A reply at the new UART_CUR baud can look like framing errors at
        // the old rate. During rate detection, keep the bounded probe sequence
        // moving instead of repeatedly resetting a successfully changed modem.
        if (op_ == Op::BaudSet) {
            start_baud_verify(true);
        } else if (phase_ == Phase::ProbeQuiet || phase_ == Phase::Probe ||
                   phase_ == Phase::EscapeBefore || phase_ == Phase::EscapeAfter ||
                   phase_ == Phase::ResetWait || phase_ == Phase::BaudQuiet ||
                   phase_ == Phase::BaudVerify || phase_ == Phase::RetryWait) {
            reset_parser();
        } else recover(framing_fault_reason_);
    }
    if (recovery_pending_) {
        // Complete any promised CIPSEND body, even after a lost receive byte,
        // before sending recovery AT commands. The copied body has fixed size.
        if (op_ == Op::SendPrompt || op_ == Op::SendBody) {
            if (op_ == Op::SendPrompt) {
                if (command_written_ != command_length_) {
                    const auto remaining = command_length_ - command_written_;
                    const auto count = io_.write(io_.context,
                        reinterpret_cast<const std::uint8_t*>(command_) + command_written_, remaining);
                    command_written_ += std::min(count, remaining);
                    if (command_written_ != command_length_ && !elapsed(now_, due_)) return;
                    if (command_written_ != command_length_) {
                        perform_recovery();
                        return;
                    }
                }
                send_written_ = 0;
                due_ = now_ + 3000;
            }
            if (op_ == Op::SendBody && elapsed(now_, due_)) {
                perform_recovery();
                return;
            }
            abandon_send_ = true;
            phase_ = Phase::RetryWait;
            op_ = Op::SendBody;
            recovery_pending_ = false;
            close_all();
        } else {
            perform_recovery();
            return;
        }
    }
    if (op_ != Op::None) {
        if (op_ == Op::SendBody) {
            const auto remaining = send_length_ - send_written_;
            const auto count = io_.write(io_.context, sending_ + send_written_, std::min<std::size_t>(remaining, 256));
            send_written_ += std::min(count, remaining);
            if (send_written_ == send_length_) {
                op_ = Op::SendResult;
                due_ = now_ + (operation_disconnected_ ? CLOSED_RESULT_GRACE_MS : 5000);
            }
        } else if (command_written_ < command_length_) {
            const auto remaining = command_length_ - command_written_;
            const auto count = io_.write(io_.context,
                reinterpret_cast<const std::uint8_t*>(command_) + command_written_, remaining);
            command_written_ += std::min(count, remaining);
            if (command_written_ == command_length_) due_ = now_ + timeout_ms_;
        } else if (op_ == Op::Escape && io_.tx_idle(io_.context)) {
            op_ = Op::None;
            phase_ = Phase::EscapeAfter;
            due_ = now_ + 1100;
        }
        if (op_ != Op::None && elapsed(now_, due_)) timeout();
        return;
    }
    switch (phase_) {
    case Phase::ProbeQuiet:
        if (elapsed(now_, due_) && io_.tx_idle(io_.context)) {
            phase_ = Phase::Probe;
            issue(Op::Probe, "\r\nAT\r\n", 750);
        }
        break;
    case Phase::EscapeBefore:
        if (elapsed(now_, due_)) issue(Op::Escape, "+++", 1000);
        break;
    case Phase::EscapeAfter:
        if (elapsed(now_, due_)) {
            phase_ = Phase::Probe;
            issue(Op::Probe, "\r\nAT\r\n", 750);
        }
        break;
    case Phase::ResetWait:
        if (elapsed(now_, due_)) {
            probe_attempt_ = 0;
            probe_attempt();
        }
        break;
    case Phase::BaudQuiet:
        if (elapsed(now_, due_) && io_.tx_idle(io_.context)) {
            phase_ = Phase::BaudVerify;
            issue(Op::BaudVerify, "\r\nAT\r\n", 750);
        }
        break;
    case Phase::Configure: configure(); break;
    case Phase::Serving:
        if (!control_active_ && connection_turns_ >= MAX_CONNECTION_TURNS) service_network_jobs();
        if (op_ == Op::None) service_connections();
        if (op_ == Op::None) service_network_jobs();
        if (op_ == Op::None) service_legacy_udp();
        break;
    case Phase::RetryWait:
        if (elapsed(now_, due_)) {
            probe_attempt_ = 0;
            probe_attempt();
        }
        break;
    default: break;
    }
}

Transport::Status Engine::status() const {
    Transport::Status value;
    value.ready = started_ && phase_ == Phase::Serving && !recovery_pending_ &&
        !abandon_send_ && !framing_fault_pending_;
    value.control_ready = value.ready && !initial_network_pending_ && !station_operation() &&
        !mdns_stop_pending_ && !mdns_pending_ && op_ != Op::ConnectionSync;
    if (!value.control_ready) {
        if (!value.ready) value.control_waiting_for = "radio_starting";
        else if (op_ == Op::StationSync || op_ == Op::ScanSync || op_ == Op::ConnectionSync)
            value.control_waiting_for = "command_sync";
        else if (op_ == Op::ScanOptions || op_ == Op::Scan) value.control_waiting_for = "scan";
        else if (op_ == Op::StationMdnsStop || op_ == Op::StationMdns || mdns_stop_pending_ || mdns_pending_)
            value.control_waiting_for = "mdns";
        else if (op_ == Op::StationAddress) value.control_waiting_for = "station_address";
        else if (op_ == Op::StationReconnectPolicy) value.control_waiting_for = "station_setup";
        else value.control_waiting_for = "station_join";
    }
    value.name = "esp8285-at";
    value.error = error_;
    value.delivery = Transport::Delivery::ModuleAccepted;
    value.bluetooth_available = false;
    value.uart_baud = baud_;
    value.modem_firmware = modem_firmware_;
    value.recommended_input_hz = baud_ >= FAST_BAUD ? 60 : 30;
    value.ap_active = value.ready && ap_enabled_;
    value.station_configured = station_configured();
    value.station_connected = value.ready && station_linked_ && station_ip_[0];
    value.station_joining = value.ready && station_joining_;
    value.station_retry_pending = station_retry_pending_;
    value.station_ip = station_ip_;
    value.hostname = config_.hostname;
    value.mdns_available = value.ready && mdns_available_;
    value.mdns_state = mdns_state();
    value.mdns_error = mdns_error_;
    value.mdns_attempts = mdns_attempts_;
    value.station_error = station_error_;
    value.station_reconnects = station_reconnects_;
    value.recoveries = recoveries_;
    value.command_timeouts = command_timeouts_;
    value.udp_beacons = legacy_udp_beacons_;
    value.udp_discovery_state = !station_configured() ? "waiting_for_station" :
        legacy_udp_disabled_ ? "disabled" : legacy_udp_ready_ ? "active" :
        op_ == Op::LegacyUdpOpen ? "opening" :
        !station_linked_ || !station_ip_[0] ? "waiting_for_station" : "settling";
    return value;
}

} // namespace NetworkService::EspAt

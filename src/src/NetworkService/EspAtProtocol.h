#pragma once

#include <cstddef>
#include <cstdint>

#include "NetworkService/Transport.h"
#include "NetworkService/WifiScan.h"

// Portable, allocation-free ESP8266/ESP8285 AT framing and command engine.
// Hardware access and application callbacks run on the transport core. The UART
// IRQ only fills a receive ring; it never enters this parser or the HTTP layer.
namespace NetworkService::EspAt {

class Parser {
public:
    struct Sink {
        void* context;
        void (*line)(void*, const char*, std::size_t);
        void (*payload)(void*, unsigned, const std::uint8_t*, std::size_t);
        void (*fault)(void*, const char*);
    };

    void reset(const Sink& sink);
    void feed(const std::uint8_t* bytes, std::size_t length);
    bool awaiting_payload() const { return remaining_ != 0; }

private:
    bool ipd_header();
    void fail(const char* reason);

    Sink sink_{};
    char line_[256]{};
    std::size_t used_ = 0;
    std::uint32_t remaining_ = 0;
    unsigned link_ = 0;
    bool skip_prompt_space_ = false;
    bool broken_ = false;
    bool discarding_scan_line_ = false;
};

class Engine {
public:
    struct Io {
        void* context;
        // Nonblocking: copy to hardware FIFO, returning the number accepted.
        std::size_t (*write)(void*, const std::uint8_t*, std::size_t);
        bool (*tx_idle)(void*);
        void (*set_baud)(void*, std::uint32_t);
        void (*discard_rx)(void*);
    };

    bool begin(const Transport::Config& config, const Transport::Callbacks& callbacks,
               const Io& io, std::uint32_t base_baud, std::uint32_t now_ms);
    void feed(const std::uint8_t* bytes, std::size_t length);
    void poll(std::uint32_t now_ms);
    void set_control_active(bool active) { control_active_ = active; }
    bool retry_connection();
    bool retry_mdns();
    bool request_scan();
    const WifiScan::Status& scan_status() const { return scan_.status(); }
    void receive_overrun();
    std::size_t write(Transport::Handle handle, const std::uint8_t* bytes, std::size_t length);
    void close(Transport::Handle handle, bool abort);
    Transport::Status status() const;

private:
    static constexpr unsigned LINK_COUNT = 5;
    static constexpr std::size_t TX_CAPACITY = 2048;
    static constexpr std::size_t SEND_CAPACITY = Transport::MAX_WRITE_SIZE;
    static constexpr std::uint32_t FAST_BAUD = 460800;

    enum class Phase : std::uint8_t {
        Stopped, ProbeQuiet, Probe, EscapeBefore, EscapeAfter,
        ResetWait, Configure, BaudQuiet, BaudVerify, Serving, RetryWait
    };
    enum class Op : std::uint8_t {
        None, Probe, Reset, Config, BaudSet, BaudVerify,
        SendPrompt, SendBody, SendResult, Close, Escape,
        StationJoin, StationAddress, StationMdnsStop, StationMdns,
        StationReconnectPolicy, StationSync, ApMode, ScanOptions, Scan, ScanSync, ConnectionSync,
        LegacyUdpOpen
    };
    enum class Step : std::uint8_t {
        Echo, Version, Store, Baud, StopServer, CloseAll, Normal, Mux,
        ActiveReceive, IpInfo, WifiMode, DisconnectStation, Hostname, Ip, AccessPoint, Dhcp,
        MaxConnections, Server, IdleTimeout, Done
    };
    struct Link {
        Transport::Handle handle = 0;
        std::uint8_t tx[TX_CAPACITY];
        std::size_t queued = 0;
        bool occupied = false;
        bool accepted = false;
        bool closing = false;
        bool abort = false;
    };

    static void on_line(void* context, const char* line, std::size_t length);
    static void on_payload(void* context, unsigned link, const std::uint8_t* bytes, std::size_t length);
    static void on_fault(void* context, const char* reason);
    void line(const char* text, std::size_t length);
    void payload(unsigned link, const std::uint8_t* bytes, std::size_t length);
    void reset_parser();
    void change_baud(std::uint32_t baud);
    void issue(Op op, const char* text, std::uint32_t timeout_ms);
    void terminal(bool success);
    void timeout();
    void probe_attempt();
    void probe_failed();
    void start_configuration();
    void configure();
    void configured_step(bool success);
    void start_baud_verify(bool fast);
    void recover(const char* reason, bool needs_reset = true);
    void perform_recovery();
    void disconnected(unsigned id);
    void connected(unsigned id);
    void notify_closed(Link& link);
    void clear_link(Link& link);
    void close_all();
    void service_connections();
    void service_legacy_udp();
    void service_station();
    void service_ap_mode();
    void request_ap_recovery();
    void service_scan();
    void service_network_jobs();
    void synchronize(Op operation);
    void station_terminal(bool success, bool timed_out = false);
    bool station_configured() const;
    bool station_operation() const;
    const char* mdns_state() const;
    void mdns_failed(const char* reason);
    Link* find(Transport::Handle handle);
    void complete_send(bool success);
    const char* step_error() const;

    Parser parser_;
    WifiScan::Cache scan_;
    Transport::Config config_{};
    Transport::Callbacks callbacks_{};
    Io io_{};
    Link links_[LINK_COUNT]{};
    std::uint8_t sending_[SEND_CAPACITY]{};
    char command_[256]{};
    char station_ip_[16]{};
    char mdns_ip_[16]{};
    char modem_firmware_[96]{};
    std::size_t command_length_ = 0;
    std::size_t command_written_ = 0;
    std::size_t send_length_ = 0;
    std::size_t send_written_ = 0;
    Transport::Handle operation_handle_ = 0;
    Transport::Handle next_handle_ = 1;
    std::uint32_t now_ = 0;
    std::uint32_t last_rx_ = 0;
    std::uint32_t due_ = 0;
    std::uint32_t timeout_ms_ = 0;
    std::uint32_t base_baud_ = 115200;
    std::uint32_t baud_ = 115200;
    std::uint32_t station_due_ = 0;
    std::uint32_t ap_mode_due_ = 0;
    std::uint32_t address_due_ = 0;
    std::uint32_t legacy_udp_due_ = 0;
    std::uint32_t legacy_udp_beacon_due_ = 0;
    std::uint32_t legacy_udp_beacons_ = 0;
    std::uint32_t mdns_due_ = 0;
    std::uint32_t mdns_attempts_ = 0;
    std::uint32_t station_reconnects_ = 0;
    std::uint32_t recoveries_ = 0;
    std::uint32_t command_timeouts_ = 0;
    const char* error_ = "";
    const char* station_error_ = "";
    const char* mdns_error_ = "";
    Phase phase_ = Phase::Stopped;
    Op op_ = Op::None;
    Step step_ = Step::Echo;
    unsigned variant_ = 0;
    unsigned probe_attempt_ = 0;
    unsigned operation_link_ = 0;
    unsigned next_link_ = 0;
    bool started_ = false;
    bool reset_after_probe_ = true;
    bool fast_disabled_ = false;
    bool verifying_fast_ = false;
    bool abandon_send_ = false;
    bool operation_disconnected_ = false;
    bool saw_no_change_ = false;
    bool framing_fault_pending_ = false;
    const char* framing_fault_reason_ = "";
    bool recovery_pending_ = false;
    bool recovery_needs_reset_ = true;
    bool station_linked_ = false;
    bool station_join_confirmed_ = false;
    bool station_joining_ = false;
    bool station_retry_pending_ = false;
    bool control_active_ = false;
    bool station_join_plain_ = false;
    bool station_join_reported_error_ = false;
    bool station_address_pending_ = false;
    unsigned station_address_variant_ = 0;
    bool station_address_seen_ = false;
    bool ap_enabled_ = false;
    bool legacy_udp_ready_ = false;
    bool legacy_udp_disabled_ = false;
    bool legacy_udp_sending_ = false;
    bool ap_recovery_pending_ = false;
    bool ap_mode_target_enabled_ = false;
    unsigned ap_mode_variant_ = 0;
    bool initial_network_pending_ = false;
    bool mdns_pending_ = false;
    bool mdns_stop_pending_ = false;
    bool mdns_available_ = false;
    unsigned mdns_failures_ = 0;
    bool reconnect_policy_pending_ = false;
    unsigned scan_step_ = 0;
    bool scan_had_rows_ = false;
    bool scan_parsed_rows_ = false;
    bool sync_version_seen_ = false;
    bool sync_busy_ = false;
    bool sync_retried_ = false;
    unsigned connection_turns_ = 0;
};

} // namespace NetworkService::EspAt

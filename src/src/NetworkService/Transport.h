#pragma once

#include <cstddef>
#include <cstdint>
#include "NetworkService/WifiScan.h"

namespace NetworkService::Transport {

constexpr std::size_t MAX_CONNECTIONS = 3;
// One complete TCP MSS per write on Pico 2 W reduces the number of pbuf
// fragments needed to deliver the large embedded WebUI.  Keep 1024-byte
// chunks on the RP2040/ESP8285 paths for their existing memory and UART limits.
#if defined(CONFIG_OGXM_BOARD_PI_PICO2W)
constexpr std::size_t MAX_WRITE_SIZE = 1460;
#else
constexpr std::size_t MAX_WRITE_SIZE = 1024;
#endif

// Nonzero connection generation, not a modem link ID or reusable array index.
// A new connection must get a new handle, including after a modem restart.
using Handle = std::uint32_t;
constexpr Handle INVALID_HANDLE = 0;

enum class Delivery : std::uint8_t {
    PeerAcknowledged, // lwIP tcp_sent: the remote TCP peer acknowledged bytes.
    ModuleAccepted,   // ESP SEND OK: the modem accepted bytes; no peer ACK proof.
};

struct Config {
    const char *ssid;
    const char *password;
    std::uint8_t channel;
    // An empty station SSID keeps AP-only operation. All strings remain valid
    // for the lifetime of the transport; settings change through save/reboot.
    const char *station_ssid = "";
    const char *station_password = "";
    const char *hostname = "dc-ogx-w";
    // Optional Pico ROM-derived identifier, stable for UDP discovery.
    const char *device_id = "";
};

struct Callbacks {
    // False rejects a connection. No received/sent callbacks after rejection.
    bool (*connected)(Handle);
    // Borrowed bytes remain valid only for the duration of the callback.
    void (*received)(Handle, const std::uint8_t *, std::size_t);
    // Completion of bytes previously accepted by write(), exactly once/in order.
    void (*sent)(Handle, std::size_t, Delivery);
    // Exactly once for an accepted connection. Old handles never target new ones.
    void (*closed)(Handle);
    // Optional TCP half-close notification. Queued output may still be delivered.
    // A modem that only exposes full CLOSED events need not invoke this callback.
    void (*read_closed)(Handle) = nullptr;
};

struct Status {
    bool ready = false;
    // AP/HTTP may already be usable while the ESP finishes its first station
    // and name-service handshake. Do not grant a new controller lease until
    // this is true; an existing lease still defers disruptive radio jobs.
    bool control_ready = true;
    const char *control_waiting_for = "";
    const char *name = "unknown";
    const char *error = ""; // Fixed ASCII status code, or an empty string.
    Delivery delivery = Delivery::ModuleAccepted;
    bool bluetooth_available = false;
    std::uint32_t uart_baud = 0;
    const char *modem_firmware = ""; // Bounded printable AT version, when available.
    std::uint16_t recommended_input_hz = 60;
    // SoftAP stays available until station DHCP succeeds, then goes into
    // standby. It returns automatically after a sustained station outage.
    bool ap_active = false;
    bool station_configured = false;
    bool station_connected = false; // True only after an IPv4 address is known.
    bool station_joining = false;
    bool station_retry_pending = false;
    const char *station_ip = "";
    const char *ap_ip = "192.168.4.1";
    const char *hostname = "dc-ogx-w";
    // Optional Pico ROM-derived identifier, stable for UDP discovery.
    const char *device_id = "";
    bool mdns_available = false; // Responder accepted/started, not DNS reachability.
    const char *mdns_state = "not_configured";
    const char *mdns_error = ""; // Rejection/timeout is not proof of missing firmware support.
    std::uint32_t mdns_attempts = 0;
    const char *station_error = "";
    std::uint32_t station_reconnects = 0;
    std::uint32_t recoveries = 0;
    std::uint32_t command_timeouts = 0; // Runtime AT command deadlines, including scoped recovery.
    const char* udp_discovery_state = "not_supported";
    std::uint32_t udp_beacons = 0;
};

// start() reports that initialization was accepted. A nonblocking transport may
// become ready later; status().ready reports an operational AP and TCP server.
// Config strings have static lifetime. Callbacks and mutable state are confined
// to the radio/transport core. The CYW43 backend also serializes them through its
// existing SDK async context; the ESP backend invokes callbacks only from poll().
bool start(const Config &config, const Callbacks &callbacks);
void poll(std::uint32_t now_ms);

// Update before poll() with actual control ownership (or another operation that
// must keep the radio/AT pipe available). Passive UI sockets do not block joins.
void set_control_active(bool active);
// Queue one attempt using the saved, active configuration. Repeated requests
// coalesce; joining/connected stations are already satisfied. An active control
// session defers the attempt until released. False means no station is configured.
bool retry_connection();
// Queue a responder restart on the active station address. An existing control
// lease defers it; repeated requests coalesce. False means no usable station.
bool retry_mdns();

// Asynchronous nearby-network discovery, sharing the same control/update guard
// as joining. Accepted/running requests coalesce. False means radio not ready.
bool request_scan();
// Borrowed bounded cache; read only in this transport's serialized context.
const WifiScan::Status& scan_status();

// Copies up to length bytes into bounded transport storage. Zero means retry
// after backpressure. sent() must never be invoked synchronously from write().
std::size_t write(Handle handle, const std::uint8_t *bytes, std::size_t length);
void flush(Handle handle);

// Begins a close (or abort). closed() may run synchronously. If the underlying
// close needs retrying, the transport keeps its slot until it has completed.
void close(Handle handle, bool abort = false);
Status status();

// CYW43 installs its BTstack timer after the existing run loop is initialized.
// ESP implementations leave this empty; the clone runtime calls service poll().
void start_radio_poll();

} // namespace NetworkService::Transport

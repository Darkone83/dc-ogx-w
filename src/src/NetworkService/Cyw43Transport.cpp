#include "Transport.h"
#include "NetworkService.h"
#include "NetworkService/UdpDiscovery.h"
#include "NetworkService/WifiScan.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/mdns.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"

extern "C" {
#include "dhcpserver.h"
}

namespace NetworkService::Transport {
namespace {

struct Socket {
    tcp_pcb *pcb{};
    Handle handle{};
    Handle aborted_handle{};
    bool accepted{};
    bool close_requested{};
};

Socket sockets[MAX_CONNECTIONS];
Callbacks callbacks{};
Handle next_handle{1};
tcp_pcb *listener{};
dhcp_server_t dhcp{};
bool server_ready{};
const char *last_error = "";
btstack_timer_source_t service_timer{};
bool timer_started{};
Config network_config{};
bool station_enabled{};
bool station_connected{};
bool ap_active{};
std::uint32_t ap_cutover_due{};
std::uint32_t ap_restore_due{};
// Hysteresis avoids dropping/recreating the DHCP server on a brief radio blip.
constexpr std::uint32_t AP_CUTOVER_MS = 2500;
constexpr std::uint32_t AP_RECOVERY_MS = 8000;
bool station_joining{};
bool station_retry_pending{};
bool control_active{};
char station_ip[16]{};
const char* station_error = "";
std::uint32_t station_due{};
std::uint32_t station_reconnects{};
bool mdns_initialized{};
bool mdns_ap{};
bool mdns_sta{};
bool mdns_ap_confirmed{};
bool mdns_sta_confirmed{};
bool mdns_retry_pending{};
const char* mdns_ap_error = "";
const char* mdns_sta_error = "";
std::uint32_t mdns_attempts{};
WifiScan::Cache scan_cache;
std::uint32_t scan_due{};
constexpr std::uint32_t SCAN_TIMEOUT_MS = 20000;
// Keep HTTP/WS servicing at 10 ms, but do not run station/AP maintenance
// and scan/status checks 100 times per second on the shared CYW43 BT radio.
constexpr std::uint32_t WIFI_MAINTENANCE_PERIOD_MS = 100;
std::uint32_t wifi_maintenance_due{};


bool due(std::uint32_t now, std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

void mdns_result(netif* interface, u8_t result, s8_t) {
    if (interface == &cyw43_state.netif[CYW43_ITF_AP]) {
        mdns_ap_confirmed = result == MDNS_PROBING_SUCCESSFUL;
        mdns_ap_error = mdns_ap_confirmed ? "" : "mdns_name_conflict";
    }
    if (interface == &cyw43_state.netif[CYW43_ITF_STA]) {
        mdns_sta_confirmed = result == MDNS_PROBING_SUCCESSFUL;
        mdns_sta_error = mdns_sta_confirmed ? "" : "mdns_name_conflict";
    }
}

bool add_mdns(netif* interface) {
    if (mdns_attempts != UINT32_MAX) ++mdns_attempts;
    const char*& error = interface == &cyw43_state.netif[CYW43_ITF_STA] ? mdns_sta_error : mdns_ap_error;
    if (mdns_resp_add_netif(interface, network_config.hostname) != ERR_OK) {
        error = "mdns_start_failed";
        return false;
    }
    error = "";
    // Hostname resolution still works if allocating the optional DNS-SD
    // service fails. The UI reports only the actual responder/probing state.
    mdns_resp_add_service(interface, network_config.hostname, "_http",
                         DNSSD_PROTO_TCP, 80, nullptr, nullptr);
    return true;
}

void restart_mdns(netif* interface) {
    if (mdns_attempts != UINT32_MAX) ++mdns_attempts;
    if (interface == &cyw43_state.netif[CYW43_ITF_STA]) {
        mdns_sta_confirmed = false;
        mdns_sta_error = "";
    } else {
        mdns_ap_confirmed = false;
        mdns_ap_error = "";
    }
    mdns_resp_restart(interface);
}

void poll_mdns() {
    if (!mdns_retry_pending || control_active || station_joining ||
        cyw43_wifi_scan_active(&cyw43_state)) return;
    if (station_enabled) {
        if (!station_connected) return;
        auto* interface = &cyw43_state.netif[CYW43_ITF_STA];
        if (mdns_sta) restart_mdns(interface);
        else mdns_sta = add_mdns(interface);
    } else {
        auto* interface = &cyw43_state.netif[CYW43_ITF_AP];
        if (mdns_ap) restart_mdns(interface);
        else mdns_ap = add_mdns(interface);
    }
    mdns_retry_pending = false;
}

WifiScan::Security scan_security(std::uint8_t value) {
    // SDK 2.3's scan parser exposes privacy/WPA/RSN bits. RSN is not a precise
    // WPA3/enterprise/cipher detector; the UI labels it as WPA2/RSN on CYW43.
    if (value & ~7u) return WifiScan::Security::Unknown;
    if ((value & 6u) == 6u) return WifiScan::Security::WpaWpa2;
    if (value & 4u) return WifiScan::Security::Wpa2;
    if (value & 2u) return WifiScan::Security::Wpa;
    if (value & 1u) return WifiScan::Security::Wep;
    return WifiScan::Security::Open;
}

int scan_result(void*, const cyw43_ev_scan_result_t* result) {
    if (!result || !result->ssid_len || result->ssid_len > 32 ||
        result->channel < 1 || result->channel > 14 ||
        std::memchr(result->ssid, 0, result->ssid_len)) return 0;
    WifiScan::Network network{};
    std::memcpy(network.ssid, result->ssid, result->ssid_len);
    std::memcpy(network.bssid, result->bssid, sizeof(network.bssid));
    network.rssi = result->rssi;
    network.channel = static_cast<std::uint8_t>(result->channel);
    network.security = scan_security(result->auth_mode);
    scan_cache.add(network);
    return 0;
}

void poll_scan(std::uint32_t now) {
    const auto phase = scan_cache.status().phase;
    if (phase == WifiScan::Phase::Running) {
        // The public SDK completion signal is scan_active becoming false;
        // result_cb is not called with nullptr when the scan finishes.
        if (!cyw43_wifi_scan_active(&cyw43_state)) scan_cache.complete(now);
        else if (due(now, scan_due)) scan_cache.fail("scan_timeout", now);
        return;
    }
    if (phase != WifiScan::Phase::Queued || control_active || station_joining ||
        cyw43_wifi_scan_active(&cyw43_state)) return;
    if (!server_ready) { scan_cache.fail("scan_radio_not_ready", now); return; }
    cyw43_wifi_scan_options_t options{};
    scan_cache.start();
    scan_due = now + SCAN_TIMEOUT_MS;
    if (cyw43_wifi_scan(&cyw43_state, &options, nullptr, scan_result))
        scan_cache.fail("scan_start_failed", now);
}

void poll_station(std::uint32_t now) {
    if (!station_enabled || !server_ready) return;
    auto* interface = &cyw43_state.netif[CYW43_ITF_STA];
    const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (link == CYW43_LINK_UP && !ip4_addr_isany_val(*netif_ip4_addr(interface))) {
        char address[16]{};
        ip4addr_ntoa_r(netif_ip4_addr(interface), address, sizeof(address));
        const bool changed = !station_connected || std::strcmp(address, station_ip);
        station_connected = true;
        if (changed) {
            netif_set_default(interface);
            if (ap_active) ap_cutover_due = now + AP_CUTOVER_MS;
        }
        ap_restore_due = 0;
        station_joining = false;
        station_retry_pending = false;
        station_error = "";
        std::memcpy(station_ip, address, sizeof(station_ip));
        if (!mdns_sta && (!mdns_sta_error[0] || changed)) mdns_sta = add_mdns(interface);
        else if (mdns_sta && changed) restart_mdns(interface);
        return;
    }
    if (station_connected) {
        station_connected = false;
        station_ip[0] = '\0';
        mdns_sta_confirmed = false;
        station_error = "station_disconnected";
        station_due = now + 20000;
        if (!ap_active) ap_restore_due = now + AP_RECOVERY_MS;
    }
    if (station_joining) {
        if (link < 0 || due(now, station_due)) {
            station_joining = false;
            station_error = link == CYW43_LINK_BADAUTH ? "station_bad_password" :
                            link == CYW43_LINK_NONET ? "station_not_found" :
                            link == CYW43_LINK_NOIP ? "station_dhcp_timeout" :
                            link < 0 ? "station_join_failed" : "station_join_timeout";
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            station_due = now + 20000;
            if (!ap_active && !ap_restore_due) ap_restore_due = now + AP_RECOVERY_MS;
        } else if (link == CYW43_LINK_NOIP) station_error = "station_waiting_for_address";
        return;
    }
    // Scan/join use the same radio command engine. Continue reporting an
    // existing association, but never begin joining while a scan is active.
    // On timeout leave the driver untouched; late completion can release it.
    if (scan_cache.status().phase == WifiScan::Phase::Queued ||
        scan_cache.status().phase == WifiScan::Phase::Running ||
        cyw43_wifi_scan_active(&cyw43_state)) return;
    if (!station_retry_pending && !due(now, station_due)) return;
    // Avoid repeated scans moving the shared radio away from an active AP
    // controller session. An idle UI or observer must not prevent joining.
    if (control_active) return;
    station_retry_pending = false;
    const bool encrypted = network_config.station_password[0] != '\0';
    if (station_reconnects != UINT32_MAX) ++station_reconnects;
    const int result = cyw43_arch_wifi_connect_async(network_config.station_ssid,
        encrypted ? network_config.station_password : nullptr,
        encrypted ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
    station_joining = result == 0;
    station_error = station_joining ? "station_joining" : "station_join_failed";
    station_due = now + (station_joining ? 25000 : 20000);
}

// The official Pico W has separate netifs, but both share one CYW43 radio.
// Leave SoftAP available for provisioning; after successful DHCP, disable it
// and deinit its AP-only DHCP/mDNS services. Restore after a sustained loss.
// This runs in radio_tick's existing CYW43 async context, never from USB IRQ.
void poll_ap_mode(std::uint32_t now) {
    if (!station_enabled || !server_ready) return;
    // A remote-control lease must not keep SoftAP broadcasting after STA
    // association and DHCP have succeeded.  AP clients will need to use the
    // station IP after cutover; the TCP listener continues on the STA netif.
    if (station_connected && ap_active && due(now, ap_cutover_due)) {
        if (mdns_ap) mdns_resp_remove_netif(&cyw43_state.netif[CYW43_ITF_AP]);
        mdns_ap = mdns_ap_confirmed = false;
        mdns_ap_error = "";
        if (dhcp.udp) dhcp_server_deinit(&dhcp);
        cyw43_arch_disable_ap_mode();
        netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
        ap_active = false;
        ap_restore_due = 0;
    } else if (!station_connected && !ap_active && ap_restore_due && due(now, ap_restore_due)) {
        // AP recovery is lower priority than controller input / station join.
        // Unlike cutover, it can safely wait until these operations finish.
        if (control_active || station_joining || cyw43_wifi_scan_active(&cyw43_state))
            return;
        // Re-enable the AP only while station access is unavailable. Keep
        // the HTTP listener bound to ANY so it serves both interfaces.
        cyw43_wifi_ap_set_channel(&cyw43_state, network_config.channel);
        cyw43_arch_enable_ap_mode(network_config.ssid, network_config.password,
                                  CYW43_AUTH_WPA2_AES_PSK);
        ip4_addr_t address, mask, gateway;
        IP4_ADDR(&address, 192, 168, 4, 1);
        IP4_ADDR(&mask, 255, 255, 255, 0);
        IP4_ADDR(&gateway, 0, 0, 0, 0);
        auto* interface = &cyw43_state.netif[CYW43_ITF_AP];
        netif_set_hostname(interface, network_config.hostname);
        netif_set_addr(interface, &address, &mask, &gateway);
        netif_set_default(interface);
        dhcp_server_init(&dhcp, interface, &address, &mask);
        mdns_ap = add_mdns(interface);
        ap_active = dhcp.udp != nullptr;
        if (!ap_active) {
            last_error = "ap_dhcp_restore_failed";
            if (mdns_ap) mdns_resp_remove_netif(interface);
            mdns_ap = mdns_ap_confirmed = false;
            cyw43_arch_disable_ap_mode();
            ap_restore_due = now + 30000;
        } else {
            last_error = "";
            ap_restore_due = 0;
        }
    }
}

err_t on_receive(void *arg, tcp_pcb *pcb, pbuf *packet, err_t error);
err_t on_sent(void *arg, tcp_pcb *pcb, u16_t length);
err_t on_poll(void *arg, tcp_pcb *pcb);
void on_error(void *arg, err_t error);

Socket *find_socket(Handle handle) {
    if (!handle)
        return nullptr;
    for (auto &socket : sockets) {
        if (socket.handle == handle && socket.pcb)
            return &socket;
    }
    return nullptr;
}

Handle allocate_handle() {
    Handle handle;
    do {
        handle = next_handle++;
    } while (!handle || find_socket(handle));
    return handle;
}

void detach_callbacks(tcp_pcb *pcb) {
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
    tcp_err(pcb, nullptr);
}

void attach_callbacks(Socket &socket, tcp_pcb *pcb) {
    tcp_arg(pcb, &socket);
    tcp_recv(pcb, on_receive);
    tcp_sent(pcb, on_sent);
    tcp_poll(pcb, on_poll, 2);
    tcp_err(pcb, on_error);
}

void release_socket(Socket &socket) {
    const Handle handle = socket.handle;
    const bool notify = socket.accepted;
    socket.pcb = nullptr;
    socket.handle = INVALID_HANDLE;
    socket.accepted = false;
    socket.close_requested = false;
    if (notify && callbacks.closed)
        callbacks.closed(handle);
}

void abort_socket(Socket &socket) {
    if (!socket.pcb)
        return;
    tcp_pcb *pcb = socket.pcb;
    detach_callbacks(pcb);
    socket.aborted_handle = socket.handle;
    tcp_abort(pcb);
    release_socket(socket);
}

void close_socket(Socket &socket) {
    if (!socket.pcb)
        return;
    tcp_pcb *pcb = socket.pcb;
    // tcp_close can free the PCB or retain it while its FIN is acknowledged.
    // Detach first so an old PCB can never call into a reused Socket workspace.
    detach_callbacks(pcb);
    if (tcp_close(pcb) == ERR_OK) {
        release_socket(socket);
    } else {
        socket.close_requested = true;
        attach_callbacks(socket, pcb);
    }
}

err_t callback_result(const Socket &socket, Handle handle) {
    // A clean close still returns ERR_OK. lwIP requires ERR_ABRT only if this
    // callback actually aborted its own connection.
    return socket.aborted_handle == handle ? ERR_ABRT : ERR_OK;
}

err_t on_receive(void *arg, tcp_pcb *pcb, pbuf *packet, err_t error) {
    auto *socket = static_cast<Socket *>(arg);
    if (!socket || socket->pcb != pcb) {
        if (packet)
            pbuf_free(packet);
        return ERR_OK;
    }
    const Handle handle = socket->handle;
    if (error != ERR_OK) {
        if (packet)
            pbuf_free(packet);
        abort_socket(*socket);
        return ERR_ABRT;
    }
    if (!packet) {
        if (callbacks.read_closed)
            callbacks.read_closed(handle);
        else
            close_socket(*socket);
        return callback_result(*socket, handle);
    }

    // Accept the pbuf before application callbacks: they may close or abort
    // this PCB. Forward its segments without allocating a second RX buffer.
    tcp_recved(pcb, packet->tot_len);
    for (const pbuf *part = packet; part; part = part->next) {
        if (socket->handle != handle || socket->pcb != pcb || socket->close_requested)
            break;
        if (part->len && callbacks.received) {
            callbacks.received(handle, static_cast<const uint8_t *>(part->payload), part->len);
        }
    }
    pbuf_free(packet);
    return callback_result(*socket, handle);
}

err_t on_sent(void *arg, tcp_pcb *pcb, u16_t length) {
    auto *socket = static_cast<Socket *>(arg);
    if (!socket || socket->pcb != pcb)
        return ERR_OK;
    const Handle handle = socket->handle;
    if (length && callbacks.sent)
        callbacks.sent(handle, length, Delivery::PeerAcknowledged);
    return callback_result(*socket, handle);
}

err_t on_poll(void *arg, tcp_pcb *pcb) {
    auto *socket = static_cast<Socket *>(arg);
    if (!socket || socket->pcb != pcb)
        return ERR_OK;
    const Handle handle = socket->handle;
    if (socket->close_requested)
        close_socket(*socket);
    return callback_result(*socket, handle);
}

void on_error(void *arg, err_t) {
    // lwIP has already freed the PCB before this callback.
    if (auto *socket = static_cast<Socket *>(arg))
        release_socket(*socket);
}

err_t on_accept(void *, tcp_pcb *pcb, err_t error) {
    if (!pcb)
        return ERR_VAL;
    if (error != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    for (auto &socket : sockets) {
        if (socket.pcb)
            continue;
        socket.pcb = pcb;
        socket.handle = allocate_handle();
        socket.aborted_handle = INVALID_HANDLE;
        socket.accepted = false;
        socket.close_requested = false;
        tcp_setprio(pcb, TCP_PRIO_MIN);
        tcp_nagle_disable(pcb);
        attach_callbacks(socket, pcb);
        if (callbacks.connected && callbacks.connected(socket.handle)) {
            socket.accepted = true;
            return ERR_OK;
        }
        abort_socket(socket);
        return ERR_ABRT;
    }
    tcp_abort(pcb);
    return ERR_ABRT;
}

void radio_tick(btstack_timer_source_t *timer) {
    // The SDK uses the same recursive async-context lock for BTstack and
    // lwIP. This also permits raw lwIP calls made by the common service poll.
    cyw43_arch_lwip_begin();
    NetworkService::poll();
    cyw43_arch_lwip_end();
    btstack_run_loop_set_timer(timer, 10);
    btstack_run_loop_add_timer(timer);
}

} // namespace

bool start(const Config &config, const Callbacks &sink) {
    if (server_ready)
        return true;
    callbacks = sink;
    network_config = config;
    station_enabled = config.station_ssid && config.station_ssid[0];
    last_error = "";
    // The board's Bluetooth path already initialized this physical radio.
    // Scan uses the STA radio interface even before a home SSID is configured.
    // Enabling it does not associate; poll_station still requires saved settings.
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_ap_set_channel(&cyw43_state, config.channel);
    cyw43_arch_enable_ap_mode(config.ssid, config.password, CYW43_AUTH_WPA2_AES_PSK);
    ap_active = true;
    ap_cutover_due = ap_restore_due = 0;
    // On the official Pico W, Wi-Fi and BT share one CYW43439 radio.
    // PM2 is the SDK's preferred high-throughput policy. Unlike NO_PM, it
    // permits Wi-Fi idle periods that may help BLE HID coexistence. This is a
    // deliberate A/B change, not a guarantee of Bluetooth reliability.
    const int wifi_pm_result = cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);
    if (wifi_pm_result != 0) {
        printf("[CYW43] Wi-Fi performance PM configuration failed: %d\n", wifi_pm_result);
    }

    ip4_addr_t address, mask, gateway;
    IP4_ADDR(&address, 192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 0, 0, 0, 0);
    cyw43_arch_lwip_begin();
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], config.hostname);
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_AP], config.hostname);
    // cyw43's interface initializer selects the most recently enabled interface
    // as default; LAN replies must use the station when both interfaces exist.
    if (station_enabled) netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
    else netif_set_default(&cyw43_state.netif[CYW43_ITF_AP]);
    netif_set_addr(&cyw43_state.netif[CYW43_ITF_AP], &address, &mask, &gateway);
    dhcp_server_init(&dhcp, &cyw43_state.netif[CYW43_ITF_AP], &address, &mask);
    if (!dhcp.udp) {
        last_error = "dhcp_start_failed";
    } else {
        listener = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!listener) {
            last_error = "tcp_alloc_failed";
        } else if (tcp_bind(listener, IP_ANY_TYPE, 80) != ERR_OK) {
            last_error = "tcp_bind_failed";
        } else {
            tcp_pcb *listening = tcp_listen_with_backlog(listener, MAX_CONNECTIONS);
            if (listening)
                listener = listening;
            else
                last_error = "tcp_listen_failed";
        }
    }
    server_ready = listener && dhcp.udp && !last_error[0];
    if (server_ready) {
        tcp_accept(listener, on_accept);
        if (!mdns_initialized) {
            mdns_resp_init();
            mdns_resp_register_name_result_cb(mdns_result);
            mdns_initialized = true;
        }
        mdns_ap = add_mdns(&cyw43_state.netif[CYW43_ITF_AP]);
        UdpDiscovery::start(&cyw43_state.netif[CYW43_ITF_AP],
                            &cyw43_state.netif[CYW43_ITF_STA]);
        station_due = to_ms_since_boot(get_absolute_time());
        station_error = station_enabled ? "station_starting" : "";
    } else {
        if (listener) {
            if (tcp_close(listener) != ERR_OK)
                tcp_abort(listener);
            listener = nullptr;
        }
        if (dhcp.udp)
            dhcp_server_deinit(&dhcp);
    }
    cyw43_arch_lwip_end();
    return server_ready;
}

void poll(uint32_t now_ms) {
    // Called under the SDK radio async-context lock by radio_tick().
    // TCP/HTTP is always serviced promptly, including when Bluetooth is active.
    for (auto &socket : sockets) {
        if (socket.pcb && socket.close_requested)
            close_socket(socket);
    }

    // Lower-priority Wi-Fi maintenance is not latency-sensitive, and its
    // CYW43 API calls compete with HCI processing on the shared radio bus.
    // Preserve fast controller/WebUI polling while reducing that background
    // work by 10x. Use wrap-safe arithmetic on the monotonic ms counter.
    if (static_cast<std::int32_t>(now_ms - wifi_maintenance_due) >= 0) {
        wifi_maintenance_due = now_ms + WIFI_MAINTENANCE_PERIOD_MS;
        poll_scan(now_ms);
        poll_station(now_ms);
        poll_ap_mode(now_ms);
        poll_mdns();
        UdpDiscovery::tick(now_ms, station_connected, control_active);
    }
}

void set_control_active(bool active) { control_active = active; }

bool retry_connection() {
    if (!station_enabled) return false;
    if (station_connected || station_joining) return true;
    station_retry_pending = true;
    return true;
}

bool retry_mdns() {
    if (!server_ready || !mdns_initialized || (station_enabled && !station_connected)) return false;
    mdns_retry_pending = true;
    return true;
}

bool request_scan() {
    const auto phase = scan_cache.status().phase;
    if (phase == WifiScan::Phase::Queued || phase == WifiScan::Phase::Running) return true;
    // Do not overwrite an SDK scan that outlived our deadline. Keep serving
    // the AP and retry after its real completion (or a user-requested restart).
    if (!server_ready || cyw43_wifi_scan_active(&cyw43_state)) return false;
    scan_cache.queue();
    return true;
}

const WifiScan::Status& scan_status() { return scan_cache.status(); }

size_t write(Handle handle, const uint8_t *bytes, size_t length) {
    Socket *socket = find_socket(handle);
    if (!socket || socket->close_requested || !bytes || !length)
        return 0;
    length = std::min(length, MAX_WRITE_SIZE);
    length = std::min(length, static_cast<size_t>(TCP_MSS));
    length = std::min(length, static_cast<size_t>(tcp_sndbuf(socket->pcb)));
    if (!length)
        return 0;
    const err_t error =
        tcp_write(socket->pcb, bytes, static_cast<u16_t>(length), TCP_WRITE_FLAG_COPY);
    return error == ERR_OK ? length : 0;
}

void flush(Handle handle) {
    if (Socket *socket = find_socket(handle))
        tcp_output(socket->pcb);
}

void close(Handle handle, bool abort) {
    if (Socket *socket = find_socket(handle)) {
        if (abort)
            abort_socket(*socket);
        else
            close_socket(*socket);
    }
}

Status status() {
    Status value;
    value.ready = server_ready;
    value.name = "cyw43";
    value.error = last_error;
    value.delivery = Delivery::PeerAcknowledged;
    value.bluetooth_available = true;
    value.recommended_input_hz = 60;
    value.ap_active = ap_active;
    value.station_configured = station_enabled;
    value.station_connected = station_connected;
    value.station_joining = station_joining;
    value.station_retry_pending = station_retry_pending;
    value.station_ip = station_ip;
    value.hostname = network_config.hostname;
    value.mdns_available = station_enabled ? (station_connected && mdns_sta_confirmed) : mdns_ap_confirmed;
    value.control_ready = server_ready && !station_joining && !cyw43_wifi_scan_active(&cyw43_state);
    value.control_waiting_for = !server_ready ? "radio_starting" : station_joining ? "station_join" :
        cyw43_wifi_scan_active(&cyw43_state) ? "scan" : "";
    value.mdns_error = station_enabled ? mdns_sta_error : mdns_ap_error;
    value.mdns_attempts = mdns_attempts;
    value.mdns_state = station_enabled && !station_connected ? "waiting_for_station" :
        mdns_retry_pending ? (control_active ? "deferred" : "queued") :
        value.mdns_available ? "active" : value.mdns_error[0] ? "failed" : "starting";
    value.station_error = station_error;
    value.station_reconnects = station_reconnects;
    return value;
}

void start_radio_poll() {
    if (timer_started)
        return;
    timer_started = true;
    service_timer.process = radio_tick;
    btstack_run_loop_set_timer(&service_timer, 10);
    btstack_run_loop_add_timer(&service_timer);
}

} // namespace NetworkService::Transport

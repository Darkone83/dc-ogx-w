#include "NetworkService/UdpDiscovery.h"
#include "NetworkService/DiscoveryProtocol.h"
#include "Board/Config.h"

#include <cstring>
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "pico/unique_id.h"

namespace UdpDiscovery {
namespace {
udp_pcb* listener{};
netif* ap_interface{};
netif* station_interface{};
char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1]{};
std::uint32_t next_beacon{};

bool valid_interface(const netif* iface) {
    return iface && netif_is_up(iface) && !ip4_addr_isany_val(*netif_ip4_addr(iface));
}

void emit(const ip_addr_t* destination, std::uint16_t port, netif* iface) {
    if (!listener || !destination || !valid_interface(iface)) return;
    char local_address[16]{};
    ip4addr_ntoa_r(netif_ip4_addr(iface), local_address, sizeof(local_address));
    char payload[160]{};
#if defined(CONFIG_OGXM_BOARD_PI_PICO2W)
    constexpr const char* board = "pico2_w";
#else
    constexpr const char* board = "pico_w";
#endif
    const std::size_t length = DiscoveryProtocol::make_announcement(
        payload, sizeof(payload), id, board, local_address);
    if (!length) return;
    pbuf* packet = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(length), PBUF_RAM);
    if (!packet) return;
    if (pbuf_take(packet, payload, static_cast<u16_t>(length)) == ERR_OK)
        (void)udp_sendto_if(listener, packet, destination, port, iface);
    pbuf_free(packet);
}

void incoming(void*, udp_pcb*, pbuf* packet, const ip_addr_t* sender, u16_t sender_port) {
    if (!packet) return;
    constexpr std::size_t kProbeLen = sizeof(DiscoveryProtocol::kProbe) - 1;
    std::uint8_t query[kProbeLen]{};
    const bool match = packet->tot_len == kProbeLen &&
        pbuf_copy_partial(packet, query, static_cast<u16_t>(kProbeLen), 0) == kProbeLen &&
        DiscoveryProtocol::is_probe(query, kProbeLen);
    // The buffer and source netif are valid for the duration of this callback.
    if (match && sender && sender_port)
        emit(sender, sender_port, ip_current_input_netif());
    pbuf_free(packet);
}

void beacon(netif* iface) {
    if (!valid_interface(iface)) return;
    ip_addr_t broadcast;
    // Directed subnet broadcast rather than a multicast dependency. Clients
    // can also send the exact query to discover on demand.
    const std::uint32_t address = ip4_addr_get_u32(netif_ip4_addr(iface));
    const std::uint32_t mask = ip4_addr_get_u32(netif_ip4_netmask(iface));
    ip4_addr_t ipv4;
    ip4_addr_set_u32(&ipv4, address | ~mask);
    ip_addr_copy_from_ip4(broadcast, ipv4);
    emit(&broadcast, DiscoveryProtocol::kPort, iface);
}
} // namespace

void start(netif* ap, netif* station) {
    if (listener) return;
    ap_interface = ap;
    station_interface = station;
    pico_get_unique_board_id_string(id, sizeof(id));
    listener = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!listener) return;
    ip_set_option(listener, SOF_BROADCAST);
    if (udp_bind(listener, IP_ANY_TYPE, DiscoveryProtocol::kPort) != ERR_OK) {
        udp_remove(listener);
        listener = nullptr;
        return;
    }
    udp_recv(listener, incoming, nullptr);
}

void tick(std::uint32_t now_ms, bool station_connected, bool remote_control_active) {
    if (!listener || remote_control_active ||
        static_cast<std::int32_t>(now_ms - next_beacon) < 0) return;
    next_beacon = now_ms + DiscoveryProtocol::kBeaconIntervalMs;
    beacon(ap_interface);
    if (station_connected) beacon(station_interface);
}
} // namespace UdpDiscovery

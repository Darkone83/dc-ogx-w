#pragma once
#include <cstdint>
struct netif;

// Native CYW43/lwIP discovery backend (official Pico W). Always optional:
// failure to bind UDP never interrupts USB or HTTP controller operation.
namespace UdpDiscovery {
void start(netif* ap, netif* station);
void tick(std::uint32_t now_ms, bool station_connected, bool remote_control_active);
} // namespace UdpDiscovery

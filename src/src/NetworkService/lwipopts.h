#pragma once

// Three client slots, up to two WebSockets, one controller owner. Bounds are
// intentional: network allocation must leave room for Bluetooth and USB.
#define NO_SYS                          1
#define LWIP_SOCKET                    0
#define LWIP_NETCONN                   0
#define LWIP_IPV4                      1
#define LWIP_IPV6                      0
#define LWIP_ARP                       1
#define LWIP_ETHERNET                  1
#define LWIP_ICMP                      1
#define LWIP_RAW                       0
#define LWIP_UDP                       1
#define LWIP_TCP                       1
#define LWIP_DHCP                      1
#define LWIP_DNS                       0
#define LWIP_AUTOIP                    0
#define LWIP_IGMP                      1
#define LWIP_MDNS_RESPONDER            1
#define LWIP_MDNS_SEARCH               0
#define LWIP_NUM_NETIF_CLIENT_DATA     1
#define MDNS_MAX_SERVICES              1
#define MDNS_MAX_STORED_PKTS           2
#define LWIP_NETIF_EXT_STATUS_CALLBACK 1
#define LWIP_NETIF_HOSTNAME            1
#define LWIP_NETIF_STATUS_CALLBACK     1
#define LWIP_NETIF_LINK_CALLBACK       1
#define LWIP_NETIF_REMOVE_CALLBACK     1
#define LWIP_NETIF_TX_SINGLE_PBUF       1
#define MEM_ALIGNMENT                  4
#define MEM_SIZE                       (16 * 1024)
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_PCB_LISTEN         1
#define MEMP_NUM_UDP_PCB                4
#define MEMP_NUM_TCP_SEG               24
#define MEMP_NUM_SYS_TIMEOUT          20
#define PBUF_POOL_SIZE                  8
#define PBUF_POOL_BUFSIZE               1536
#define TCP_MSS                        1460
#define TCP_WND                        (2 * TCP_MSS)
#define TCP_SND_BUF                    (2 * TCP_MSS)
#define TCP_SND_QUEUELEN               12
#define TCP_LISTEN_BACKLOG              1
#define LWIP_TCP_KEEPALIVE              1
#define LWIP_STATS                     0
#define MEM_STATS                      0
#define SYS_STATS                      0
#define MEMP_STATS                     0
#define LWIP_CHKSUM_ALGORITHM           3
#define LWIP_CHECKSUM_CTRL_PER_NETIF    0
#define DHCP_DOES_ARP_CHECK             0
#define LWIP_TIMEVAL_PRIVATE            0
#define LWIP_DEBUG                     0

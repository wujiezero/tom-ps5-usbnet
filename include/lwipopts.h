/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H
#define NO_SYS 0
#define SYS_LIGHTWEIGHT_PROT 1
#define LWIP_TIMERS 1
#define LWIP_TCPIP_CORE_LOCKING 1
#define LWIP_COMPAT_SOCKETS 0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_SOCKET 1
#define LWIP_NETCONN 1
#define LWIP_NETCONN_SEM_PER_THREAD 0
#define LWIP_SOCKET_SELECT 1
#define LWIP_SOCKET_POLL 1
#define LWIP_SO_RCVTIMEO 1
#define LWIP_SO_SNDTIMEO 1
#define LWIP_SO_SNDRCVTIMEO_NONSTANDARD 0
#define LWIP_SO_LINGER 1
#define LWIP_TCP 1
#define LWIP_UDP 0
#define LWIP_RAW 0
#define LWIP_DNS 0
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ICMP 0
#define LWIP_IGMP 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define IP_FORWARD 0
#define IP_REASSEMBLY 0
#define IP_FRAG 0
#define LWIP_NETIF_LOOPBACK 0
#define LWIP_HAVE_LOOPIF 0
#define MEM_ALIGNMENT 8
#define MEM_SIZE (1024 * 1024)
#define MEMP_NUM_PBUF 128
#define MEMP_NUM_TCP_PCB 48
#define MEMP_NUM_TCP_PCB_LISTEN 4
#define MEMP_NUM_TCP_SEG 256
#define MEMP_NUM_NETCONN 64
#define PBUF_POOL_SIZE 256
#define PBUF_POOL_BUFSIZE 1600
#define TCP_MSS 1460
#define TCP_WND (32 * TCP_MSS)
#define TCP_SND_BUF (32 * TCP_MSS)
#define TCP_SND_QUEUELEN 128
#define TCP_LISTEN_BACKLOG 1
#define TCPIP_MBOX_SIZE 256
/* Every queued frame holds one of these. lwIP's default of 8 is far below TCP_WND,
 * so a burst of received frames is dropped before TCP sees it. */
#define MEMP_NUM_TCPIP_MSG_INPKT TCPIP_MBOX_SIZE
#define DEFAULT_TCP_RECVMBOX_SIZE 128
#define DEFAULT_ACCEPTMBOX_SIZE 32
#define DEFAULT_THREAD_STACKSIZE 0
#define TCPIP_THREAD_STACKSIZE 0
#define TCPIP_THREAD_PRIO 0
#define LWIP_STATS 0
/* LWIP_PROVIDE_ERRNO must remain undefined: upstream tests it with #ifdef. */
#define LWIP_ERRNO_STDINCLUDE 1
#define LWIP_SOCKET_OFFSET 0
#define LWIP_CHECKSUM_CTRL_PER_NETIF 0
#define LWIP_RAND() un_port_rand()
unsigned int un_port_rand(void);
/* The console's network: an address from the LAN, and buffers for a 10 GbE link.
 * The TCP window is 2 MiB, so a stall of about 1.6 ms still does not idle the link. */
#undef LWIP_UDP
#undef LWIP_DHCP
#undef LWIP_AUTOIP
#undef LWIP_ICMP
#undef MEM_SIZE
#undef MEMP_NUM_PBUF
#undef MEMP_NUM_TCP_SEG
#undef PBUF_POOL_SIZE
#undef TCP_WND
#undef TCP_SND_BUF
#undef TCP_SND_QUEUELEN
#undef TCP_LISTEN_BACKLOG
#undef DEFAULT_TCP_RECVMBOX_SIZE
#undef LWIP_STATS
#define LWIP_UDP 1
#define LWIP_DHCP 1
#define LWIP_AUTOIP 1
#define LWIP_DHCP_AUTOIP_COOP 1
#define LWIP_DHCP_AUTOIP_COOP_TRIES 3
#define LWIP_ICMP 1
#define MEM_SIZE (16 * 1024 * 1024)
/* Allocated at run time, so a refusal is a line in the log and not a payload that does
 * not start. */
#define LWIP_RAM_HEAP_POINTER net_heap
extern unsigned char *net_heap;
#define MEMP_NUM_PBUF 512
#define MEMP_NUM_TCP_SEG 4096
#define PBUF_POOL_SIZE 4096
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 6
#define TCP_WND (2 * 1024 * 1024)
#define TCP_SND_BUF (2 * 1024 * 1024)
#define TCP_SND_QUEUELEN 4096
/* Upstream's timers lose a segment for 1.5 s at best, and 3 s before the first round
 * trip is measured. Timeouts are in slow ticks, twice this, and bottom out at three of
 * them. */
#define TCP_TMR_INTERVAL 50
#define LWIP_TCP_RTO_TIME 1000
#define TCP_SNDLOWAT (32 * 1024)
#define TCP_LISTEN_BACKLOG 1
#define DEFAULT_TCP_RECVMBOX_SIZE 4096
#define LWIP_TCP_SACK_OUT 1
#define LWIP_CHECKSUM_ON_COPY 1
#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 0
#define MIB2_STATS 1
#define SYS_STATS 0
#endif

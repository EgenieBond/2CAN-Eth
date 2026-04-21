/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : Target/lwipopts.h
  * Description        : This file overrides LwIP stack default configuration
  *                      done in opt.h file.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion --------------------------------------*/
#ifndef __LWIPOPTS__H__
#define __LWIPOPTS__H__

#include "main.h"

/*-----------------------------------------------------------------------------*/
/* Current version of LwIP supported by CubeMx: 2.1.2 -*/
/*-----------------------------------------------------------------------------*/

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */
#include <stdint.h>

/* These symbols are defined in STM32H723ZGTX_FLASH.ld (.lwip_heap section) */
extern uint8_t __lwip_heap_start__;
extern uint8_t __lwip_heap_end__;
/* USER CODE END 0 */

#ifdef __cplusplus
extern "C" {
#endif

/* STM32CubeMX Specific Parameters (not defined in opt.h) ---------------------*/
#define WITH_RTOS 1
#define CHECKSUM_BY_HARDWARE 0

/* LwIP Stack Parameters ------------------------------------------------------*/
#define ETH_RX_BUFFER_SIZE                1536
#define MEM_ALIGNMENT                     4
#define LWIP_RAM_HEAP_POINTER             ((uint32_t)&__lwip_heap_start__)
#define LWIP_SUPPORT_CUSTOM_PBUF          1
#define LWIP_ETHERNET                     1
#define LWIP_DNS_SECURE                   7

#define TCP_SND_QUEUELEN                  9
#define TCP_SNDLOWAT                      1071
#define TCP_SNDQUEUELOWAT                 5
#define TCP_WND_UPDATE_THRESHOLD          536

#define LWIP_NETIF_LINK_CALLBACK          1
#define TCPIP_THREAD_STACKSIZE            8192
#define TCPIP_THREAD_PRIO                 ((osPriority_t)osPriorityAboveNormal)
#define TCPIP_MBOX_SIZE                   32
#define SLIPIF_THREAD_STACKSIZE           1024
#define SLIPIF_THREAD_PRIO                3
#define DEFAULT_THREAD_STACKSIZE          4096
#define DEFAULT_THREAD_PRIO               3
#define DEFAULT_UDP_RECVMBOX_SIZE         16
#define DEFAULT_TCP_RECVMBOX_SIZE         16
#define DEFAULT_ACCEPTMBOX_SIZE           8
#define RECV_BUFSIZE_DEFAULT              2000000000
#define LWIP_STATS                        0

/* ===== Checksums =====
 * software generation оставляем включённой,
 * а входную проверку IP/ICMP временно отключаем для диагностики
 */
#define CHECKSUM_GEN_IP                   1
#define CHECKSUM_GEN_UDP                  1
#define CHECKSUM_GEN_TCP                  1
#define CHECKSUM_GEN_ICMP                 1
#define CHECKSUM_GEN_ICMP6                1

#define CHECKSUM_CHECK_IP                 0
#define CHECKSUM_CHECK_UDP                1
#define CHECKSUM_CHECK_TCP                1
#define CHECKSUM_CHECK_ICMP               0
#define CHECKSUM_CHECK_ICMP6              1

/* USER CODE BEGIN 1 */
/* ===== IPv4 fragmentation ===== */
#define IP_REASSEMBLY                   0
#define IP_FRAG                         0
#define MEMP_NUM_REASSDATA              0
#define IP_REASS_MAX_PBUFS              0

/* ===== Static IPv4 ===== */
#define IP_ADDR0                          10
#define IP_ADDR1                          0
#define IP_ADDR2                          0
#define IP_ADDR3                          100

#define NETMASK_ADDR0                     255
#define NETMASK_ADDR1                     255
#define NETMASK_ADDR2                     255
#define NETMASK_ADDR3                     0

#define GW_ADDR0                          10
#define GW_ADDR1                          0
#define GW_ADDR2                          0
#define GW_ADDR3                          1

/* Alignment padding for Ethernet frames */
#define ETH_PAD_SIZE                      2

/* ===== Core protocol features ===== */
#define LWIP_ARP                          1
#define LWIP_ETHERNET                     1
#define LWIP_RAW                          1
#define LWIP_TCP                          1
#define LWIP_UDP                          1
#define LWIP_ICMP                         1
#define LWIP_DHCP                         0
#define LWIP_AUTOIP                       0

/* raw API only */
#define LWIP_NETCONN                      0
#define LWIP_SOCKET                       0

/* ===== Ping behavior ===== */
#define LWIP_BROADCAST_PING               1
#define LWIP_MULTICAST_PING               1

/* ===== TCP resources ===== */
#define MEMP_NUM_TCP_PCB                  10
#define MEMP_NUM_TCP_PCB_LISTEN           6
#define MEMP_NUM_TCP_SEG                  32
#define MEMP_NUM_TCPIP_MSG_INPKT          32
#define MEMP_NUM_SYS_TIMEOUT              10

#define TCP_MSS                           1460
#define TCP_SND_BUF                       (4 * TCP_MSS)
#define TCP_WND                           (4 * TCP_MSS)

/* ===== ARP ===== */
#define ARP_TABLE_SIZE                    10
#define ARP_QUEUEING                      0

/* ===== Netif callbacks ===== */
#define LWIP_NETIF_LINK_CALLBACK          1
#define LWIP_NETIF_STATUS_CALLBACK        1

/* ===== Debug ===== */
#define LWIP_DEBUG                        0
//#define LWIP_DBG_TYPES_ON                 (LWIP_DBG_ON | LWIP_DBG_LEVEL_ALL)
#define LWIP_DBG_TYPES_ON                 LWIP_DBG_OFF

#define ETHARP_DEBUG                      LWIP_DBG_OFF
#define NETIF_DEBUG                       LWIP_DBG_OFF
#define IP_DEBUG                          LWIP_DBG_OFF
#define ICMP_DEBUG                        LWIP_DBG_OFF
#define PBUF_DEBUG                        LWIP_DBG_OFF
#define TCPIP_DEBUG                       LWIP_DBG_OFF

#ifndef LWIP_PLATFORM_DIAG
#define LWIP_PLATFORM_DIAG(x) do { DebugUART_Print x; } while(0)
#endif

#ifndef LWIP_PLATFORM_ASSERT
#define LWIP_PLATFORM_ASSERT(x) do { DebugUART_Print("[LWIP-ASSERT] %s\r\n", x); for(;;); } while(0)
#endif

/* USER CODE END 1 */

#ifdef __cplusplus
}
#endif
#endif /* __LWIPOPTS__H__ */

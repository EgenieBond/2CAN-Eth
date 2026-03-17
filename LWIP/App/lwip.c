/* USER CODE BEGIN Header */
/**
 ******************************************************************************
  * File Name          : LWIP.c
  * Description        : This file provides initialization code for LWIP
  *                      middleWare.
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

/* Includes ------------------------------------------------------------------*/
#include "lwip.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#if defined ( __CC_ARM )  /* MDK ARM Compiler */
#include "lwip/sio.h"
#endif /* MDK ARM Compiler */
#include "ethernetif.h"
#include <string.h>

/* USER CODE BEGIN 0 */
#include "debug_uart.h"
/* USER CODE END 0 */
/* Private function prototypes -----------------------------------------------*/
static void ethernet_link_status_updated(struct netif *netif);
/* ETH Variables initialization ----------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/* Variables Initialization */
struct netif gnetif;
ip4_addr_t ipaddr;
ip4_addr_t netmask;
ip4_addr_t gw;
uint8_t IP_ADDRESS[4];
uint8_t NETMASK_ADDRESS[4];
uint8_t GATEWAY_ADDRESS[4];
/* USER CODE BEGIN OS_THREAD_ATTR_CMSIS_RTOS_V2 */
#define INTERFACE_THREAD_STACK_SIZE ( 2048 )
osThreadAttr_t attributes;
/* USER CODE END OS_THREAD_ATTR_CMSIS_RTOS_V2 */

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */

/**
  * LwIP initialization function
  */
void MX_LWIP_Init(void)
{
  /* IPv4 static config */
  IP4_ADDR(&ipaddr,  IP_ADDR0,  IP_ADDR1,  IP_ADDR2,  IP_ADDR3);
  IP4_ADDR(&netmask, NETMASK_ADDR0, NETMASK_ADDR1, NETMASK_ADDR2, NETMASK_ADDR3);
  IP4_ADDR(&gw,      GW_ADDR0, GW_ADDR1, GW_ADDR2, GW_ADDR3);

  /* Init TCP/IP stack */
  tcpip_init(NULL, NULL);

  /* Add network interface */
  netif_add(&gnetif,
            &ipaddr,
            &netmask,
            &gw,
            NULL,
            &ethernetif_init,
            &tcpip_input);

  DebugUART_Print("[LWIP] gnetif added\r\n");

  /* Make it default */
  netif_set_default(&gnetif);
  DebugUART_Print("[LWIP] netif_set_default done\r\n");

  /*
   * IMPORTANT:
   * Do NOT force netif UP here unconditionally.
   * Real link state is controlled by ethernet_link_thread().
   */
  netif_set_down(&gnetif);
  netif_set_link_down(&gnetif);
  DebugUART_Print("[LWIP] netif forced DOWN initially\r\n");

  /* Link/status callback */
  netif_set_link_callback(&gnetif, ethernet_link_status_updated);

  /* Create Ethernet link handler thread */
  memset(&attributes, 0x0, sizeof(osThreadAttr_t));
  attributes.name = "EthLink";
  attributes.stack_size = INTERFACE_THREAD_STACK_SIZE;
  attributes.priority = osPriorityBelowNormal;
  osThreadNew(ethernet_link_thread, &gnetif, &attributes);

  DebugUART_Print("[LWIP] Ethernet link thread created\r\n");

  DebugUART_Print("[LWIP] IP: %d.%d.%d.%d\r\n",
                  ip4_addr1(netif_ip4_addr(&gnetif)),
                  ip4_addr2(netif_ip4_addr(&gnetif)),
                  ip4_addr3(netif_ip4_addr(&gnetif)),
                  ip4_addr4(netif_ip4_addr(&gnetif)));

  DebugUART_Print("[LWIP] NETMASK: %d.%d.%d.%d\r\n",
                  ip4_addr1(netif_ip4_netmask(&gnetif)),
                  ip4_addr2(netif_ip4_netmask(&gnetif)),
                  ip4_addr3(netif_ip4_netmask(&gnetif)),
                  ip4_addr4(netif_ip4_netmask(&gnetif)));

  DebugUART_Print("[LWIP] GW: %d.%d.%d.%d\r\n",
                  ip4_addr1(netif_ip4_gw(&gnetif)),
                  ip4_addr2(netif_ip4_gw(&gnetif)),
                  ip4_addr3(netif_ip4_gw(&gnetif)),
                  ip4_addr4(netif_ip4_gw(&gnetif)));
}

#ifdef USE_OBSOLETE_USER_CODE_SECTION_4
/* Kept to help code migration. (See new 4_1, 4_2... sections) */
/* Avoid to use this user section which will become obsolete. */
/* USER CODE BEGIN 4 */
/* USER CODE END 4 */
#endif

/**
  * @brief  Notify the User about the network interface config status
  * @param  netif: the network interface
  * @retval None
  */
static void ethernet_link_status_updated(struct netif *netif)
{
  if (netif_is_up(netif))
  {
    DebugUART_Print("[LWIP] netif is UP\r\n");
  }
  else
  {
    DebugUART_Print("[LWIP] netif is DOWN\r\n");
  }

  if (netif_is_link_up(netif))
    DebugUART_Print("[LWIP] link is UP\r\n");
  else
    DebugUART_Print("[LWIP] link is DOWN\r\n");
}

#if defined ( __CC_ARM )  /* MDK ARM Compiler */
/**
 * Opens a serial device for communication.
 *
 * @param devnum device number
 * @return handle to serial device if successful, NULL otherwise
 */
sio_fd_t sio_open(u8_t devnum)
{
  sio_fd_t sd;

/* USER CODE BEGIN 7 */
  sd = 0; // dummy code
/* USER CODE END 7 */

  return sd;
}

/**
 * Sends a single character to the serial device.
 *
 * @param c character to send
 * @param fd serial device handle
 *
 * @note This function will block until the character can be sent.
 */
void sio_send(u8_t c, sio_fd_t fd)
{
/* USER CODE BEGIN 8 */
/* USER CODE END 8 */
}

/**
 * Reads from the serial device.
 *
 * @param fd serial device handle
 * @param data pointer to data buffer for receiving
 * @param len maximum length (in bytes) of data to receive
 * @return number of bytes actually received - may be 0 if aborted by sio_read_abort
 *
 * @note This function will block until data can be received. The blocking
 * can be cancelled by calling sio_read_abort().
 */
u32_t sio_read(sio_fd_t fd, u8_t *data, u32_t len)
{
  u32_t recved_bytes;

/* USER CODE BEGIN 9 */
  recved_bytes = 0; // dummy code
/* USER CODE END 9 */
  return recved_bytes;
}

/**
 * Tries to read from the serial device. Same as sio_read but returns
 * immediately if no data is available and never blocks.
 *
 * @param fd serial device handle
 * @param data pointer to data buffer for receiving
 * @param len maximum length (in bytes) of data to receive
 * @return number of bytes actually received
 */
u32_t sio_tryread(sio_fd_t fd, u8_t *data, u32_t len)
{
  u32_t recved_bytes;

/* USER CODE BEGIN 10 */
  recved_bytes = 0; // dummy code
/* USER CODE END 10 */
  return recved_bytes;
}
#endif /* MDK ARM Compiler */


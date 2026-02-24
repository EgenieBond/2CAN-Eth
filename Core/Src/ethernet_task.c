/*
 * ethernet_task.c
 *
 *  Created on: Feb 4, 2026
 *      Author: Egenie
 */

#include "ethernet_task.h"
#include "main.h"
#include "lwip.h"
#include "lwip/netif.h"
#include "cmsis_os.h"
#include "debug_uart.h"
#include "raw_tcp_server.h"
#include <string.h>

#include "stm32h7xx_hal.h"   // чтобы был HAL_ETH_ReadPHYRegister
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
extern ETH_HandleTypeDef heth;

extern struct netif gnetif;
extern UART_HandleTypeDef huart3;

/* Очередь для передачи данных Ethernet -> Core */
osMessageQueueId_t eth_to_core_queue = NULL;

static void PHY_Scan(void)
{
    uint32_t id1=0, id2=0, bsr=0;
    HAL_StatusTypeDef st1, st2, st3;

    DebugUART_Print("[PHY] Scanning MDIO addresses 0..31\r\n");

    for (uint32_t addr = 0; addr < 32; addr++)
    {
        st1 = HAL_ETH_ReadPHYRegister(&heth, addr, 2, &id1); // PHYIDR1
        st2 = HAL_ETH_ReadPHYRegister(&heth, addr, 3, &id2); // PHYIDR2
        st3 = HAL_ETH_ReadPHYRegister(&heth, addr, 1, &bsr); // BSR

        DebugUART_Print("[PHY] addr=%lu st=%d/%d/%d ID1=0x%04lX ID2=0x%04lX BSR=0x%04lX\r\n",
                        addr, st1, st2, st3, id1, id2, bsr);
    }
}

/* callback in tcpip thread */
static void tcp_server_init_cb(void *arg)
{
  (void)arg;
  DebugUART_Print("[TCP] init cb: running in tcpip_thread\r\n");
  RawTcpServer_Init();
  DebugUART_Print("[TCP] init cb: RawTcpServer_Init done\r\n");
}

void EthernetTask(void *argument)
{
  (void)argument;

  const char *boot = "\r\n[ETH] EthernetTask ENTER (HAL_UART_Transmit)\r\n";
  HAL_UART_Transmit(&huart3, (uint8_t*)boot, (uint16_t)strlen(boot), 100);

  DebugUART_Print("[ETH] Ethernet task started (DebugUART)\r\n");

  /* 1) Ждём, пока интерфейс administratively UP */
  while (!netif_is_up(&gnetif))
  {
    DebugUART_Print("[ETH] waiting netif UP...\r\n");
    osDelay(200);
  }
  DebugUART_Print("[ETH] netif UP\r\n");

  /* 2) Печатаем IP сразу (он уже задан статически) */
  DebugUART_Print("[ETH] My IP: %d.%d.%d.%d\r\n",
                  ip4_addr1(netif_ip4_addr(&gnetif)),
                  ip4_addr2(netif_ip4_addr(&gnetif)),
                  ip4_addr3(netif_ip4_addr(&gnetif)),
                  ip4_addr4(netif_ip4_addr(&gnetif)));

  /* 3) СТАРТУЕМ сервер СРАЗУ, не ждём линк */
  DebugUART_Print("[ETH] starting RAW TCP server via tcpip_callback...\r\n");
  err_t e = tcpip_callback(tcp_server_init_cb, NULL);
  DebugUART_Print("[ETH] tcpip_callback returned %d\r\n", (int)e);

  DebugUART_Print("[ETH] try: telnet %d.%d.%d.%d 2001\r\n",
                  ip4_addr1(netif_ip4_addr(&gnetif)),
                  ip4_addr2(netif_ip4_addr(&gnetif)),
                  ip4_addr3(netif_ip4_addr(&gnetif)),
                  ip4_addr4(netif_ip4_addr(&gnetif)));

  /* 4) Heartbeat: мониторим линк */
  for (;;)
  {
    osDelay(2000);
    DebugUART_Print("[ETH] heartbeat: link=%d up=%d flags=0x%02X\r\n",
                    netif_is_link_up(&gnetif) ? 1 : 0,
                    netif_is_up(&gnetif) ? 1 : 0,
                    (unsigned)gnetif.flags);
  }
}

/* EthernetTask_Start */
static osThreadId_t ethTaskHandle;

void EthernetTask_Start(void)
{
  const osThreadAttr_t attr = {
    .name = "EthTask",
    .stack_size = 8192,
    .priority = (osPriority_t)osPriorityAboveNormal
  };

  ethTaskHandle = osThreadNew(EthernetTask, NULL, &attr);

  if (ethTaskHandle == NULL)
    DebugUART_Print("[ETH] ERROR: osThreadNew(EthernetTask) failed\r\n");
  else
    DebugUART_Print("[ETH] EthernetTask thread created\r\n");
}

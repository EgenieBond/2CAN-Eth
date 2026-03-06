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

#include "app_queues.h"

#ifndef PHY_ADDR
#define PHY_ADDR 0U   // LAN8742 обычно на адресе 0
#endif

extern ETH_HandleTypeDef heth;

extern struct netif gnetif;
extern UART_HandleTypeDef huart3;

#include "eth_events.h"
//extern osEventFlagsId_t g_ethLinkEvt;

// счетчики
extern volatile uint32_t g_rx_irq_cnt;
extern volatile uint32_t g_rx_sem_cnt;

extern uint8_t _sbss, _ebss;
extern uint8_t __lwip_heap_start__, __lwip_heap_end__;
extern uint8_t ucHeap[];   /* если ругнётся — см. ниже примечание */

static inline uint8_t netif_flags_read_volatile(struct netif *n)
{
  return *((volatile uint8_t *)&n->flags);
}

static void DumpMemLayout(void)
{
    DebugUART_Print("[MEM] _sbss=0x%08lX _ebss=0x%08lX\r\n",
                    (uint32_t)&_sbss, (uint32_t)&_ebss);
    DebugUART_Print("[MEM] lwip_heap: 0x%08lX .. 0x%08lX\r\n",
                    (uint32_t)&__lwip_heap_start__, (uint32_t)&__lwip_heap_end__);
    DebugUART_Print("[MEM] LWIP_RAM_HEAP_POINTER=0x%08lX\r\n",
                    (uint32_t)LWIP_RAM_HEAP_POINTER);
}

static void PHY_Dump(void)
{
  uint32_t v = 0;

  DebugUART_Print("\r\n===== PHY BASIC =====\r\n");

  /* 0: BMCR */
  if (HAL_ETH_ReadPHYRegister(&heth, PHY_ADDR, 0, &v) == HAL_OK)
  {
    DebugUART_Print("BMCR(0) = 0x%04lX  [RST=%lu AN_EN=%lu SPEED100=%lu DUPLEX=%lu]\r\n",
      v,
      (v >> 15) & 1,  /* reset */
      (v >> 12) & 1,  /* auto-neg enable */
      (v >> 13) & 1,  /* speed select (100) */
      (v >> 8) & 1    /* duplex */
    );
  }

  /* 1: BSR (read twice because it's latched) */
  uint32_t bsr1=0, bsr2=0;
  HAL_ETH_ReadPHYRegister(&heth, PHY_ADDR, 1, &bsr1);
  HAL_ETH_ReadPHYRegister(&heth, PHY_ADDR, 1, &bsr2);
  DebugUART_Print("BSR(1)  = 0x%04lX -> 0x%04lX  [LINK=%lu AN_DONE=%lu]\r\n",
    bsr1, bsr2,
    (bsr2 >> 2) & 1,
    (bsr2 >> 5) & 1
  );

  /* 2/3: PHY ID */
  uint32_t id1=0, id2=0;
  HAL_ETH_ReadPHYRegister(&heth, PHY_ADDR, 2, &id1);
  HAL_ETH_ReadPHYRegister(&heth, PHY_ADDR, 3, &id2);
  DebugUART_Print("PHYIDR1(2)=0x%04lX  PHYIDR2(3)=0x%04lX\r\n", id1, id2);

  /* 4/5: AN advertise / link partner ability */
  uint32_t anar=0, anlpar=0;
  HAL_ETH_ReadPHYRegister(&heth, PHY_ADDR, 4, &anar);
  HAL_ETH_ReadPHYRegister(&heth, PHY_ADDR, 5, &anlpar);
  DebugUART_Print("ANAR(4)=0x%04lX  ANLPAR(5)=0x%04lX\r\n", anar, anlpar);

  DebugUART_Print("===== PHY DUMP 0..31 (raw) =====\r\n");
  for (uint32_t reg = 0; reg < 32; reg++)
  {
    if (HAL_ETH_ReadPHYRegister(&heth, PHY_ADDR, reg, &v) == HAL_OK)
      DebugUART_Print("PHY[%02lu]=0x%04lX\r\n", reg, v);
    else
      DebugUART_Print("PHY[%02lu]=<read fail>\r\n", reg);
  }
}

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

//-------------------------------------------------------------------------------
void EthernetTask(void *argument)
{
  (void)argument;

  const char *boot = "\r\n[ETH] EthernetTask ENTER (HAL_UART_Transmit)\r\n";
  HAL_UART_Transmit(&huart3, (uint8_t*)boot, (uint16_t)strlen(boot), 100);

  DebugUART_Print("[ETH] Ethernet task started (DebugUART)\r\n");

  DumpMemLayout();

  /* 1) Ждём LINK_UP по событию (если оно есть), иначе fallback polling */
DebugUART_Print("[ETH] waiting LINK UP (event)... evt=%p mask=0x%08lX\r\n",
                (void*)g_ethLinkEvt, (unsigned long)APP_ETH_EVT_LINK_UP);

if (g_ethLinkEvt)
{
  uint32_t w = osEventFlagsWait(g_ethLinkEvt,
                               APP_ETH_EVT_LINK_UP,
                               osFlagsWaitAny,
                               osWaitForever);

  DebugUART_Print("[ETH] wait returned=0x%08lX now_get=0x%08lX\r\n",
                  (unsigned long)w,
                  (unsigned long)osEventFlagsGet(g_ethLinkEvt));

  /* если ошибка — это будет 0xFFFFFFxx */
  if ((int32_t)w < 0)
  {
    DebugUART_Print("[ETH] osEventFlagsWait ERROR: 0x%08lX\r\n", (unsigned long)w);
    /* fallback: не зависаем навсегда */
    while (!netif_is_link_up(&gnetif)) osDelay(50);
  }
  else
  {
    DebugUART_Print("[ETH] osEventFlagsWait OK: flags=0x%08lX\r\n", (unsigned long)w);
  }
}
else
{
  DebugUART_Print("[ETH] WARNING: g_ethLinkEvt is NULL -> fallback polling\r\n");
  while (!netif_is_link_up(&gnetif)) osDelay(50);
}

  DebugUART_Print("[ETH] LINK UP (task sees it)\r\n");

  /* 2) Теперь PHY дамп корректен */
  //PHY_Dump();
  //PHY_Scan();

  /* 3) IP */
  DebugUART_Print("[ETH] My IP: %d.%d.%d.%d\r\n",
                  ip4_addr1(netif_ip4_addr(&gnetif)),
                  ip4_addr2(netif_ip4_addr(&gnetif)),
                  ip4_addr3(netif_ip4_addr(&gnetif)),
                  ip4_addr4(netif_ip4_addr(&gnetif)));

  while (!netif_is_up(&gnetif) || !netif_is_link_up(&gnetif)) {
    DebugUART_Print("[ETH] waiting netif up/link... up=%d link=%d\r\n",
                    netif_is_up(&gnetif), netif_is_link_up(&gnetif));
    osDelay(100);
  }

  /* 4) TEMP: пока проверяем fake-client pipeline, TCP server не стартуем */
  DebugUART_Print("[ETH] TEMP: RAW TCP server start is disabled\r\n");

  for (;;)
  {
    osDelay(10000);

    DebugUART_Print("[ETH] heartbeat: link=%d up=%d flags=0x%02X\r\n",
                    netif_is_link_up(&gnetif) ? 1 : 0,
                    netif_is_up(&gnetif) ? 1 : 0,
                    (unsigned)gnetif.flags);

    static uint32_t last_irq = 0, last_sem = 0;

    last_irq = g_rx_irq_cnt;
    last_sem = g_rx_sem_cnt;
  }
}

// -------------------------------------------- /* EthernetTask_Start */
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

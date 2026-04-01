/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : ethernetif.c
  * Description        : This file provides code for the configuration
  *                      of the ethernetif.c MiddleWare.
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
#include "main.h"
#include "lwip/opt.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"
#include "lwip/ethip6.h"
#include "ethernetif.h"
#include "lan8742.h"
#include <string.h>
#include "cmsis_os.h"
#include "lwip/tcpip.h"

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */
#include "raw_tcp_server.h"
#include <stdint.h>
#include "lwip/prot/ip4.h"
#include "eth_events.h"
#include "debug_uart.h"
#include "lwip/etharp.h"

extern struct netif gnetif;
void invalidate_dcache_range(void *addr, uint32_t size);
static uint8_t g_MACAddr[6] = {0x00,0x80,0xE1,0x00,0x00,0x00};
//osEventFlagsId_t g_ethLinkEvt = NULL;

/* USER CODE END 0 */

/* Private define ------------------------------------------------------------*/
/* The time to block waiting for input. */
#define TIME_WAITING_FOR_INPUT ( portMAX_DELAY )
/* Time to block waiting for transmissions to finish */
#define ETHIF_TX_TIMEOUT (2000U)
/* USER CODE BEGIN OS_THREAD_STACK_SIZE_WITH_RTOS */
/* Stack size of the interface thread */
#define INTERFACE_THREAD_STACK_SIZE ( 1024 )
/* USER CODE END OS_THREAD_STACK_SIZE_WITH_RTOS */
/* Network interface name */
#define IFNAME0 's'
#define IFNAME1 't'

/* ETH Setting  */
#define ETH_DMA_TRANSMIT_TIMEOUT               ( 20U )
#define ETH_TX_BUFFER_MAX             ((ETH_TX_DESC_CNT) * 2U)
/* ETH_RX_BUFFER_SIZE parameter is defined in lwipopts.h */

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/* Private variables ---------------------------------------------------------*/
/*
@Note: This interface is implemented to operate in zero-copy mode only:
        - Rx Buffers will be allocated from LwIP stack Rx memory pool,
          then passed to ETH HAL driver.
        - Tx Buffers will be allocated from LwIP stack memory heap,
          then passed to ETH HAL driver.

@Notes:
  1.a. ETH DMA Rx descriptors must be contiguous, the default count is 4,
       to customize it please redefine ETH_RX_DESC_CNT in ETH GUI (Rx Descriptor Length)
       so that updated value will be generated in stm32xxxx_hal_conf.h
  1.b. ETH DMA Tx descriptors must be contiguous, the default count is 4,
       to customize it please redefine ETH_TX_DESC_CNT in ETH GUI (Tx Descriptor Length)
       so that updated value will be generated in stm32xxxx_hal_conf.h

  2.a. Rx Buffers number must be between ETH_RX_DESC_CNT and 2*ETH_RX_DESC_CNT
  2.b. Rx Buffers must have the same size: ETH_RX_BUFFER_SIZE, this value must
       passed to ETH DMA in the init field (heth.Init.RxBuffLen)
  2.c  The RX Ruffers addresses and sizes must be properly defined to be aligned
       to L1-CACHE line size (32 bytes).
*/

/* Data Type Definitions */
typedef enum
{
  RX_ALLOC_OK       = 0x00,
  RX_ALLOC_ERROR    = 0x01
} RxAllocStatusTypeDef;

typedef struct __attribute__((aligned(32)))
{
  struct pbuf_custom pbuf_custom;

  uint8_t pad[(32 - (sizeof(struct pbuf_custom) % 32)) % 32];

  uint8_t buff[ETH_RX_BUFFER_SIZE + ETH_PAD_SIZE];
} RxBuff_t;

/* Memory Pool Declaration */
#define ETH_RX_BUFFER_CNT             12U
LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");

/* Variable Definitions */
static uint8_t RxAllocStatus;

#if defined ( __ICCARM__ ) /*!< IAR Compiler */

#pragma location=0x30000000
ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
#pragma location=0x30000080
ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */

#elif defined ( __CC_ARM )  /* MDK ARM Compiler */

__attribute__((at(0x30000000))) ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
__attribute__((at(0x30000080))) ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */

#elif defined ( __GNUC__ ) /* GNU Compiler */

ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT] __attribute__((section(".RxDecripSection"))); /* Ethernet Rx DMA Descriptors */
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT] __attribute__((section(".TxDecripSection")));   /* Ethernet Tx DMA Descriptors */

#endif

#if defined ( __ICCARM__ ) /*!< IAR Compiler */
#pragma location = 0x30000100
extern u8_t memp_memory_RX_POOL_base[];

#elif defined ( __CC_ARM ) /* MDK ARM Compiler */
__attribute__((section(".Rx_PoolSection"))) extern u8_t memp_memory_RX_POOL_base[];

#elif defined ( __GNUC__ ) /* GNU */
__attribute__((section(".Rx_PoolSection"))) extern u8_t memp_memory_RX_POOL_base[];
#endif

/* USER CODE BEGIN 2 */
/* USER CODE END 2 */

osSemaphoreId RxPktSemaphore = NULL;   /* Semaphore to signal incoming packets */
osSemaphoreId TxPktSemaphore = NULL;   /* Semaphore to signal transmit packet complete */

/* Global Ethernet handle */
ETH_HandleTypeDef heth;
ETH_TxPacketConfig TxConfig;

/* Private function prototypes -----------------------------------------------*/
int32_t ETH_PHY_IO_Init(void);
int32_t ETH_PHY_IO_DeInit (void);
int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal);
int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal);
int32_t ETH_PHY_IO_GetTick(void);

lan8742_Object_t LAN8742;
lan8742_IOCtx_t  LAN8742_IOCtx = {ETH_PHY_IO_Init,
                                  ETH_PHY_IO_DeInit,
                                  ETH_PHY_IO_WriteReg,
                                  ETH_PHY_IO_ReadReg,
                                  ETH_PHY_IO_GetTick};

/* USER CODE BEGIN 3 */
#include "lwip/prot/ethernet.h"
#include "lwip/prot/etharp.h"
#include "lwip/prot/ip4.h"
#include "lwip/pbuf.h"
#include <stdint.h>
#include "lwip/netif.h"

//static uint8_t arp_tx_frame[60] __attribute__((aligned(32)));
static err_t low_level_output(struct netif *netif, struct pbuf *p);
static ETH_BufferTypeDef g_TxBuffer[ETH_TX_DESC_CNT] __attribute__((aligned(32)));

// глобальные счётчики
volatile uint32_t g_rx_irq_cnt = 0;
volatile uint32_t g_rx_sem_cnt = 0;
volatile uint32_t g_tx_cplt_cnt = 0;
volatile uint32_t g_tx_err_cnt  = 0;
volatile uint32_t g_rx_alloc_ok_cnt   = 0;
volatile uint32_t g_rx_alloc_fail_cnt = 0;
volatile uint32_t g_rx_link_cnt       = 0;
volatile uint32_t g_rx_read_ok_cnt    = 0;
volatile uint32_t g_rx_read_null_cnt  = 0;

volatile uint32_t g_low_level_output_cnt = 0; //счетчик TX-вызовов

/* D-Cache line size for Cortex-M7 is 32 bytes */
#define DCACHE_LINE_SIZE 32U

void invalidate_dcache_range(void *addr, uint32_t size)
{
  if (addr == NULL || size == 0U) return;

  uintptr_t start = (uintptr_t)addr;
  uintptr_t end   = start + (uintptr_t)size;

  /* Align to cache line boundaries */
  start &= ~(uintptr_t)(DCACHE_LINE_SIZE - 1U);
  end    = (end + (DCACHE_LINE_SIZE - 1U)) & ~(uintptr_t)(DCACHE_LINE_SIZE - 1U);

  SCB_InvalidateDCache_by_Addr((uint32_t*)start, (int32_t)(end - start));
}

static void clean_dcache_range(void *addr, uint32_t size)
{
  if (addr == NULL || size == 0U) return;

  uintptr_t start = (uintptr_t)addr;
  uintptr_t end   = start + (uintptr_t)size;

  start &= ~(uintptr_t)(DCACHE_LINE_SIZE - 1U);
  end    = (end + (DCACHE_LINE_SIZE - 1U)) & ~(uintptr_t)(DCACHE_LINE_SIZE - 1U);

  SCB_CleanDCache_by_Addr((uint32_t*)start, (int32_t)(end - start));
}

/* Копируем первые N байт из pbuf chain в линейный буфер */
static uint16_t pbuf_copy_head(uint8_t *dst, uint16_t dst_len, const struct pbuf *p)
{
  uint16_t copied = 0;
  const struct pbuf *q = p;

  while (q && copied < dst_len)
  {
    uint16_t to_copy = q->len;
    if ((uint32_t)copied + to_copy > dst_len)
      to_copy = (uint16_t)(dst_len - copied);

    memcpy(dst + copied, q->payload, to_copy);
    copied = (uint16_t)(copied + to_copy);
    q = q->next;
  }
  return copied;
}

/* Логируем Ethernet тип + (если IPv4) протокол/источник БЕЗ невыравненных access */
static void log_rx_pbuf(const struct pbuf *p)
{
  LWIP_UNUSED_ARG(p);
}

static void netif_link_up_in_tcpip(void *arg)
{
  struct netif *netif = (struct netif *)arg;

  if (netif == NULL)
  {
    DebugUART_Print("[ETH] netif_link_up_in_tcpip: netif=NULL\r\n");
    return;
  }

  DebugUART_Print("[ETH] netif_link_up_in_tcpip ENTER flags_before=0x%02X\r\n",
                  (unsigned)netif->flags);

  netif_set_link_up(netif);
  netif_set_up(netif);

  DebugUART_Print("[ETH] tcpip: netif link UP + netif UP\r\n");
  DebugUART_Print("[ETH] tcpip: flags_after=0x%02X\r\n", (unsigned)netif->flags);
  DebugUART_Print("[ETH] tcpip: input=%p output=%p linkoutput=%p\r\n",
                  (void *)netif->input,
                  (void *)netif->output,
                  (void *)netif->linkoutput);
  DebugUART_Print("[ETH] tcpip: hwaddr=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                  netif->hwaddr[0], netif->hwaddr[1], netif->hwaddr[2],
                  netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5]);
  DebugUART_Print("[ETH] tcpip: ip=%d.%d.%d.%d\r\n",
                  ip4_addr1(netif_ip4_addr(netif)),
                  ip4_addr2(netif_ip4_addr(netif)),
                  ip4_addr3(netif_ip4_addr(netif)),
                  ip4_addr4(netif_ip4_addr(netif)));

  if (g_ethLinkEvt)
  {
    osEventFlagsSet(g_ethLinkEvt, APP_ETH_EVT_LINK_UP);
  }

  DebugUART_Print("[ETH] netif_link_up_in_tcpip EXIT\r\n");
}

static void netif_link_down_in_tcpip(void *arg)
{
  struct netif *netif = (struct netif *)arg;

  if (netif == NULL)
  {
    DebugUART_Print("[ETH] netif_link_down_in_tcpip: netif=NULL\r\n");
    return;
  }

  DebugUART_Print("[ETH] netif_link_down_in_tcpip ENTER flags_before=0x%02X\r\n",
                  (unsigned)netif->flags);

  netif_set_link_down(netif);
  netif_set_down(netif);

  DebugUART_Print("[ETH] tcpip: netif link DOWN + netif DOWN\r\n");
  DebugUART_Print("[ETH] tcpip: flags_after=0x%02X\r\n", (unsigned)netif->flags);

  if (g_ethLinkEvt)
  {
    osEventFlagsClear(g_ethLinkEvt, APP_ETH_EVT_LINK_UP);
  }

  DebugUART_Print("[ETH] netif_link_down_in_tcpip EXIT\r\n");
}

static void Debug_PrintPhyRegs(void)
{
  uint32_t reg = 0;
  uint32_t phyAddr = 0;   // у LAN8742 обычно адрес 0

  if (HAL_ETH_ReadPHYRegister(&heth, phyAddr, 0x00, &reg) == HAL_OK)
    DebugUART_Print("[PHY] BMCR  = 0x%04lX\r\n", reg);

  if (HAL_ETH_ReadPHYRegister(&heth, phyAddr, 0x01, &reg) == HAL_OK)
    DebugUART_Print("[PHY] BMSR  = 0x%04lX\r\n", reg);

  if (HAL_ETH_ReadPHYRegister(&heth, phyAddr, 0x1F, &reg) == HAL_OK)
    DebugUART_Print("[PHY] PSCSR = 0x%04lX\r\n", reg);
}

static void log_if_arp_for_me(struct pbuf *p, struct netif *netif)
{
  if ((p == NULL) || (netif == NULL) || (p->tot_len < 42))
    return;

  uint8_t head[42];
  if (pbuf_copy_partial(p, head, sizeof(head), 0) < 42)
    return;

  uint16_t eth_type = (uint16_t)((head[12] << 8) | head[13]);
  if (eth_type != 0x0806)
    return;

  const uint8_t *arp = &head[14];

  uint16_t oper = (uint16_t)((arp[6] << 8) | arp[7]);
  if (oper != 1)
    return;

  const ip4_addr_t *my_ip = netif_ip4_addr(netif);
  uint8_t my_ip_bytes[4] = {
    ip4_addr1(my_ip),
    ip4_addr2(my_ip),
    ip4_addr3(my_ip),
    ip4_addr4(my_ip)
  };

  const uint8_t *target_ip = &arp[24];

  if (memcmp(target_ip, my_ip_bytes, 4) == 0)
  {
    static uint32_t arp_for_me_cnt = 0;
    if (arp_for_me_cnt < 20U)
    {
      arp_for_me_cnt++;
      DebugUART_Print("[RX] ARP request for me from %u.%u.%u.%u\r\n",
                      arp[14], arp[15], arp[16], arp[17]);
    }
  }
}

static int is_arp_for_me(const struct pbuf *p, struct netif *netif)
{
  if ((p == NULL) || (netif == NULL) || (p->tot_len < 42))
    return 0;

  uint8_t head[42];
  if (pbuf_copy_partial((const struct pbuf *)p, head, sizeof(head), 0) < 42)
    return 0;

  uint16_t eth_type = (uint16_t)((head[12] << 8) | head[13]);
  if (eth_type != 0x0806)   /* not ARP */
    return 0;

  const uint8_t *arp = &head[14];
  uint16_t oper = (uint16_t)((arp[6] << 8) | arp[7]);
  if (oper != 1)            /* not ARP request */
    return 0;

  const ip4_addr_t *my_ip = netif_ip4_addr(netif);
  uint8_t my_ip_bytes[4] = {
    ip4_addr1(my_ip),
    ip4_addr2(my_ip),
    ip4_addr3(my_ip),
    ip4_addr4(my_ip)
  };

  const uint8_t *target_ip = &arp[24];

  return (memcmp(target_ip, my_ip_bytes, 4) == 0) ? 1 : 0;
}

static void Debug_DumpEthRegs(const char *tag)
{
  DebugUART_Print("[%s] DMACSR=0x%08lX DMAMR=0x%08lX MACCR=0x%08lX\r\n",
                  tag,
                  (unsigned long)ETH->DMACSR,
                  (unsigned long)ETH->DMAMR,
                  (unsigned long)ETH->MACCR);

  DebugUART_Print("[%s] MTLTQDR=0x%08lX MTLRQDR=0x%08lX\r\n",
                  tag,
                  (unsigned long)ETH->MTLTQDR,
                  (unsigned long)ETH->MTLRQDR);
}

static void dump_arp_packet(const struct pbuf *p, struct netif *netif)
{
  if ((p == NULL) || (netif == NULL) || (p->tot_len < 14))
    return;

  uint8_t head[64];
  uint16_t copied = pbuf_copy_partial((const struct pbuf *)p, head, sizeof(head), 0);
  if (copied < 14)
    return;

  uint16_t eth_type = (uint16_t)((head[12] << 8) | head[13]);

  DebugUART_Print("[PKT-DUMP] p=%p len=%u tot=%u ref=%u payload=%p eth_type=0x%04X\r\n",
                  (void *)p,
                  (unsigned)p->len,
                  (unsigned)p->tot_len,
                  (unsigned)p->ref,
                  p->payload,
                  (unsigned)eth_type);

  DebugUART_Print("[PKT-DUMP] ETH dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                  head[0], head[1], head[2], head[3], head[4], head[5],
                  head[6], head[7], head[8], head[9], head[10], head[11]);

  if (eth_type != 0x0806)
  {
    if (eth_type == 0x0800 && copied >= 34)
    {
      uint8_t ip_proto = head[23];
      DebugUART_Print("[PKT-DUMP] IPv4 proto=%u src=%u.%u.%u.%u dst=%u.%u.%u.%u\r\n",
                      (unsigned)ip_proto,
                      head[26], head[27], head[28], head[29],
                      head[30], head[31], head[32], head[33]);
    }
    else if (eth_type == 0x86DD)
    {
      DebugUART_Print("[PKT-DUMP] IPv6 packet\r\n");
    }
    else
    {
      DebugUART_Print("[PKT-DUMP] non-ARP/non-IPv4 eth_type=0x%04X\r\n",
                      (unsigned)eth_type);
    }
    return;
  }

  if (copied < 42)
  {
    DebugUART_Print("[PKT-DUMP] ARP frame too short: copied=%u\r\n", (unsigned)copied);
    return;
  }

  const uint8_t *arp = &head[14];

  uint16_t htype = (uint16_t)((arp[0] << 8) | arp[1]);
  uint16_t ptype = (uint16_t)((arp[2] << 8) | arp[3]);
  uint8_t  hlen  = arp[4];
  uint8_t  plen  = arp[5];
  uint16_t oper  = (uint16_t)((arp[6] << 8) | arp[7]);

  const uint8_t *sha = &arp[8];
  const uint8_t *spa = &arp[14];
  const uint8_t *tha = &arp[18];
  const uint8_t *tpa = &arp[24];

  DebugUART_Print("[PKT-DUMP] ARP htype=%u ptype=0x%04X hlen=%u plen=%u oper=%u\r\n",
                  (unsigned)htype,
                  (unsigned)ptype,
                  (unsigned)hlen,
                  (unsigned)plen,
                  (unsigned)oper);

  DebugUART_Print("[PKT-DUMP] SHA=%02X:%02X:%02X:%02X:%02X:%02X SPA=%u.%u.%u.%u\r\n",
                  sha[0], sha[1], sha[2], sha[3], sha[4], sha[5],
                  spa[0], spa[1], spa[2], spa[3]);

  DebugUART_Print("[PKT-DUMP] THA=%02X:%02X:%02X:%02X:%02X:%02X TPA=%u.%u.%u.%u\r\n",
                  tha[0], tha[1], tha[2], tha[3], tha[4], tha[5],
                  tpa[0], tpa[1], tpa[2], tpa[3]);

  DebugUART_Print("[PKT-DUMP] my MAC=%02X:%02X:%02X:%02X:%02X:%02X my IP=%u.%u.%u.%u\r\n",
                  netif->hwaddr[0], netif->hwaddr[1], netif->hwaddr[2],
                  netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5],
                  ip4_addr1(netif_ip4_addr(netif)),
                  ip4_addr2(netif_ip4_addr(netif)),
                  ip4_addr3(netif_ip4_addr(netif)),
                  ip4_addr4(netif_ip4_addr(netif)));
}

/* USER CODE END 3 */

/* Private functions ---------------------------------------------------------*/
void pbuf_free_custom(struct pbuf *p);

/**
  * @brief  Ethernet Rx Transfer completed callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
  (void)handlerEth;
  g_rx_irq_cnt++;
  osSemaphoreRelease(RxPktSemaphore);
}

/**
  * @brief  Ethernet Tx Transfer completed callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
  uint32_t hal_err = HAL_ETH_GetError(handlerEth);
  uint32_t dma_err = HAL_ETH_GetDMAError(handlerEth);

  g_tx_cplt_cnt++;

  DebugUART_Print("[TX] HAL_ETH_TxCpltCallback ENTER cnt=%lu heth=%p hal_err=0x%08lX dma_err=0x%08lX\r\n",
                  (unsigned long)g_tx_cplt_cnt,
                  (void *)handlerEth,
                  (unsigned long)hal_err,
                  (unsigned long)dma_err);

  DebugUART_Print("[TX-CPLT] DMACSR=0x%08lX DMAMR=0x%08lX MACCR=0x%08lX\r\n",
                  (unsigned long)ETH->DMACSR,
                  (unsigned long)ETH->DMAMR,
                  (unsigned long)ETH->MACCR);

  DebugUART_Print("[TX-CPLT] MTLTQDR=0x%08lX MTLRQDR=0x%08lX\r\n",
                  (unsigned long)ETH->MTLTQDR,
                  (unsigned long)ETH->MTLRQDR);

  osStatus_t s = osSemaphoreRelease(TxPktSemaphore);
  DebugUART_Print("[TX] HAL_ETH_TxCpltCallback sem release -> %ld\r\n", (long)s);

  DebugUART_Print("[TX] HAL_ETH_TxCpltCallback EXIT\r\n");
}
/**
  * @brief  Ethernet DMA transfer error callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *handlerEth)
{
  uint32_t hal_err = HAL_ETH_GetError(handlerEth);
  uint32_t dma_err = HAL_ETH_GetDMAError(handlerEth);

  g_tx_err_cnt++;

  DebugUART_Print("[ETH] ERROR callback ENTER cnt=%lu heth=%p hal_err=0x%08lX dma_err=0x%08lX\r\n",
                  (unsigned long)g_tx_err_cnt,
                  (void *)handlerEth,
                  (unsigned long)hal_err,
                  (unsigned long)dma_err);

  DebugUART_Print("[ETH-ERR] DMACSR=0x%08lX DMAMR=0x%08lX MACCR=0x%08lX\r\n",
                  (unsigned long)ETH->DMACSR,
                  (unsigned long)ETH->DMAMR,
                  (unsigned long)ETH->MACCR);

  DebugUART_Print("[ETH-ERR] MTLTQDR=0x%08lX MTLRQDR=0x%08lX\r\n",
                  (unsigned long)ETH->MTLTQDR,
                  (unsigned long)ETH->MTLRQDR);

  osStatus_t s1 = osSemaphoreRelease(TxPktSemaphore);
  DebugUART_Print("[ETH] ERROR callback tx sem release -> %ld\r\n", (long)s1);

  if ((dma_err & ETH_DMACSR_RBU) == ETH_DMACSR_RBU)
  {
    DebugUART_Print("[ETH] ERROR callback: RBU detected -> wake RX\r\n");
    osStatus_t s2 = osSemaphoreRelease(RxPktSemaphore);
    DebugUART_Print("[ETH] ERROR callback rx sem release -> %ld\r\n", (long)s2);
  }

  DebugUART_Print("[ETH] ERROR callback EXIT\r\n");
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/*******************************************************************************
                       LL Driver Interface ( LwIP stack --> ETH)
*******************************************************************************/
/**
 * @brief In this function, the hardware should be initialized.
 * Called from ethernetif_init().
 *
 * @param netif the already initialized lwip network interface structure
 *        for this ethernetif
 */
static void low_level_init(struct netif *netif)
{
  HAL_StatusTypeDef hal_eth_init_status = HAL_OK;
  osThreadAttr_t attributes;

  uint32_t duplex = ETH_FULLDUPLEX_MODE;
  uint32_t speed  = ETH_SPEED_100M;
  int32_t PHYLinkState = 0;
  ETH_MACConfigTypeDef MACConf = {0};

  /* --- ETH HAL init --- */
  heth.Instance = ETH;
  heth.Init.MACAddr = g_MACAddr;
  heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
  heth.Init.TxDesc = DMATxDscrTab;
  heth.Init.RxDesc = DMARxDscrTab;
  heth.Init.RxBuffLen = 1536;

  LWIP_PLATFORM_DIAG(("[ETH] ethernetif.c BUILD TAG: AAA_1\r\n"));
  LWIP_PLATFORM_DIAG(("[ETH] Initializing Ethernet hardware...\r\n"));
  LWIP_PLATFORM_DIAG(("[ETH] MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                      g_MACAddr[0], g_MACAddr[1], g_MACAddr[2],
                      g_MACAddr[3], g_MACAddr[4], g_MACAddr[5]));

  hal_eth_init_status = HAL_ETH_Init(&heth);

  /* На всякий случай: если где-то MPU не применился как ожидаем,
     чистим кэш по таблицам дескрипторов (DMA читает их напрямую). */
  SCB_CleanDCache_by_Addr((uint32_t*)DMARxDscrTab, sizeof(DMARxDscrTab));
  SCB_CleanDCache_by_Addr((uint32_t*)DMATxDscrTab, sizeof(DMATxDscrTab));

  memset(&TxConfig, 0, sizeof(TxConfig));
  TxConfig.Attributes   = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
  TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
  TxConfig.CRCPadCtrl   = ETH_CRC_PAD_INSERT;

  /* RX pool init */
  LWIP_MEMPOOL_INIT(RX_POOL);

#if LWIP_ARP || LWIP_ETHERNET

  netif->hwaddr_len = ETH_HWADDR_LEN;
  memcpy(netif->hwaddr, g_MACAddr, 6);

  netif->mtu = ETH_MAX_PAYLOAD;

#if LWIP_ARP
  netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
#else
  netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHERNET;
#endif

  /* semaphores */
  RxPktSemaphore = osSemaphoreNew(1, 0, NULL);
  TxPktSemaphore = osSemaphoreNew(1, 0, NULL);

  /* create event flags once (global in eth_events.c) */
  if (g_ethLinkEvt == NULL)
  {
    g_ethLinkEvt = osEventFlagsNew(NULL);
    LWIP_PLATFORM_DIAG(("[EVT] g_ethLinkEvt created = %p\r\n", (void*)g_ethLinkEvt));
  }
  else
  {
    LWIP_PLATFORM_DIAG(("[EVT] g_ethLinkEvt already  = %p\r\n", (void*)g_ethLinkEvt));
  }

  /* sanity: print mask */
  LWIP_PLATFORM_DIAG(("[EVT] mask APP_ETH_EVT_LINK_UP=0x%08lX\r\n",
                      (unsigned long)APP_ETH_EVT_LINK_UP));

  /* input thread */
  memset(&attributes, 0, sizeof(attributes));
  attributes.name = "EthIf";
  attributes.stack_size = 4096;
  attributes.priority = osPriorityBelowNormal;
  osThreadNew(ethernetif_input, netif, &attributes);

  /* PHY init */
  LAN8742_RegisterBusIO(&LAN8742, &LAN8742_IOCtx);

  if (LAN8742_Init(&LAN8742) != LAN8742_STATUS_OK)
  {
    /* Не валим netif административно, просто link_down */
    netif_set_link_down(netif);
    LWIP_PLATFORM_DIAG(("[ETH] LAN8742_Init failed -> link down only\r\n"));
    return;
  }

  if (hal_eth_init_status != HAL_OK)
  {
    Error_Handler();
  }

  Debug_PrintPhyRegs();

  /* initial link state: only detect and pre-configure MAC,
     but DO NOT start ETH and DO NOT raise netif here.
     The only owner of link up/down must be ethernet_link_thread(). */
  PHYLinkState = LAN8742_GetLinkState(&LAN8742);
  DebugUART_Print("[ETH] PHYLinkState(initial)=%ld\r\n", (long)PHYLinkState);
  Debug_PrintPhyRegs();

  if (PHYLinkState > LAN8742_STATUS_LINK_DOWN)
  {
    switch (PHYLinkState)
    {
      case LAN8742_STATUS_100MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE;
        speed  = ETH_SPEED_100M;
        break;

      case LAN8742_STATUS_100MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE;
        speed  = ETH_SPEED_100M;
        break;

      case LAN8742_STATUS_10MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE;
        speed  = ETH_SPEED_10M;
        break;

      case LAN8742_STATUS_10MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE;
        speed  = ETH_SPEED_10M;
        break;

      default:
        duplex = ETH_FULLDUPLEX_MODE;
        speed  = ETH_SPEED_100M;
        break;
    }

    HAL_ETH_GetMACConfig(&heth, &MACConf);
    MACConf.DuplexMode = duplex;
    MACConf.Speed      = speed;
    HAL_ETH_SetMACConfig(&heth, &MACConf);

    DebugUART_Print("[MAC] Preconfig only: Speed=%s Duplex=%s\r\n",
                    (MACConf.Speed == ETH_SPEED_100M) ? "100M" : "10M",
                    (MACConf.DuplexMode == ETH_FULLDUPLEX_MODE) ? "FULL" : "HALF");
  }
  else
  {
    DebugUART_Print("[ETH] initial PHY link DOWN\r\n");
  }

  DebugUART_Print("[ETH] low_level_init done, waiting for ethernet_link_thread to control link state\r\n");

#endif /* LWIP_ARP || LWIP_ETHERNET */
}

/**
 * @brief This function should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @param p the MAC packet to send (e.g. IP packet including MAC addresses and type)
 * @return ERR_OK if the packet could be sent
 *         an err_t value if the packet couldn't be sent
 *
 * @note Returning ERR_MEM here if a DMA queue of your MAC is full can lead to
 *       strange results. You might consider waiting for space in the DMA queue
 *       to become available since the stack doesn't retry to send a packet
 *       dropped because of memory failure (except for the TCP timers).
 */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
#if ETH_PAD_SIZE
  pbuf_header(p, -ETH_PAD_SIZE);
#endif

  g_low_level_output_cnt++;

  uint32_t i = 0U;
  struct pbuf *q = NULL;

  LWIP_UNUSED_ARG(netif);

  memset(g_TxBuffer, 0, sizeof(g_TxBuffer));

  uint16_t eth_type = 0xFFFF;
  if ((p != NULL) && (p->tot_len >= 14U))
  {
    uint8_t hdr[14];
    pbuf_copy_partial(p, hdr, sizeof(hdr), 0);
    eth_type = (uint16_t)((hdr[12] << 8) | hdr[13]);

    DebugUART_Print("[TXPATH] low_level_output #%lu p=%p tot_len=%u type=0x%04X ref=%u\r\n",
                    (unsigned long)g_low_level_output_cnt,
                    (void *)p,
                    (unsigned)p->tot_len,
                    (unsigned)eth_type,
                    (unsigned)p->ref);

    DebugUART_Print("[TXPATH] dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                    hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5],
                    hdr[6], hdr[7], hdr[8], hdr[9], hdr[10], hdr[11]);
  }
  else
  {
    DebugUART_Print("[TXPATH] low_level_output #%lu invalid packet p=%p\r\n",
                    (unsigned long)g_low_level_output_cnt,
                    (void *)p);
  }

  for (q = p; q != NULL; q = q->next)
  {
    if (i >= ETH_TX_DESC_CNT)
    {
      DebugUART_Print("[TXPATH] ERR: too many pbufs in chain (%lu)\r\n",
                      (unsigned long)i);
#if ETH_PAD_SIZE
      pbuf_header(p, ETH_PAD_SIZE);
#endif
      return ERR_IF;
    }

    g_TxBuffer[i].buffer = (uint8_t *)q->payload;
    g_TxBuffer[i].len    = q->len;
    g_TxBuffer[i].next   = NULL;

    if (i > 0U)
    {
      g_TxBuffer[i - 1U].next = &g_TxBuffer[i];
    }

    i++;
  }

  for (uint32_t j = 0; j < i; j++)
  {
    clean_dcache_range(g_TxBuffer[j].buffer, g_TxBuffer[j].len);
  }
  clean_dcache_range(g_TxBuffer, sizeof(g_TxBuffer));

  TxConfig.Length   = p->tot_len;
  TxConfig.TxBuffer = g_TxBuffer;
  TxConfig.pData    = p;

  if ((eth_type == 0x0800U) || (eth_type == 0x86DDU))
  {
    TxConfig.Attributes   = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
    TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
  }
  else
  {
    TxConfig.Attributes   = ETH_TX_PACKETS_FEATURES_CRCPAD;
    TxConfig.ChecksumCtrl = ETH_CHECKSUM_DISABLE;
  }

  pbuf_ref(p);

  HAL_StatusTypeDef st = HAL_ETH_Transmit_IT(&heth, &TxConfig);

  DebugUART_Print("[TXPATH] HAL_ETH_Transmit_IT ret=%d hal_err=0x%08lX dma_err=0x%08lX\r\n",
                  (int)st,
                  (unsigned long)HAL_ETH_GetError(&heth),
                  (unsigned long)HAL_ETH_GetDMAError(&heth));

  if (st != HAL_OK)
  {
    pbuf_free(p);
#if ETH_PAD_SIZE
    pbuf_header(p, ETH_PAD_SIZE);
#endif
    return ERR_IF;
  }

  if (osSemaphoreAcquire(TxPktSemaphore, ETHIF_TX_TIMEOUT) != osOK)
  {
    DebugUART_Print("[TXPATH] timeout waiting Tx complete\r\n");
    pbuf_free(p);
#if ETH_PAD_SIZE
    pbuf_header(p, ETH_PAD_SIZE);
#endif
    return ERR_IF;
  }

#if ETH_PAD_SIZE
  pbuf_header(p, ETH_PAD_SIZE);
#endif

  return ERR_OK;
}

/**
 * @brief Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return a pbuf filled with the received packet (including MAC header)
 *         NULL on memory error
   */
static struct pbuf * low_level_input(struct netif *netif)
{
  struct pbuf *p = NULL;
  LWIP_UNUSED_ARG(netif);

  if (RxAllocStatus == RX_ALLOC_OK)
  {
    HAL_StatusTypeDef st = HAL_ETH_ReadData(&heth, (void **)&p);

    if (st == HAL_OK)
    {
      if (p != NULL)
      {
        static uint32_t rxread_ok_cnt = 0;
        rxread_ok_cnt++;
        if (rxread_ok_cnt <= 40U)
        {
          DebugUART_Print("[RXREAD] OK cnt=%lu p=%p len=%u tot_len=%u ref=%u payload=%p\r\n",
                          (unsigned long)rxread_ok_cnt,
                          (void *)p,
                          (unsigned)p->len,
                          (unsigned)p->tot_len,
                          (unsigned)p->ref,
                          p->payload);
        }

        log_if_arp_for_me(p, netif);
      }
    }
    else
    {
      static uint32_t rxread_fail_cnt = 0;
      rxread_fail_cnt++;
      if (rxread_fail_cnt <= 40U)
      {
        DebugUART_Print("[RXREAD] HAL_ETH_ReadData FAIL st=%d hal_err=0x%08lX dma_err=0x%08lX DMACSR=0x%08lX\r\n",
                        (int)st,
                        (unsigned long)HAL_ETH_GetError(&heth),
                        (unsigned long)HAL_ETH_GetDMAError(&heth),
                        (unsigned long)ETH->DMACSR);
      }
    }
  }

  return p;
}

/**
 * @brief This function should be called when a packet is ready to be read
 * from the interface. It uses the function low_level_input() that
 * should handle the actual reception of bytes from the network
 * interface. Then the type of the received packet is determined and
 * the appropriate input function is called.
 *
 * @param netif the lwip network interface structure for this ethernetif
 */
void ethernetif_input(void* argument)
{
  struct pbuf *p = NULL;
  struct netif *netif = (struct netif *)argument;

  static uint32_t dbg_pkt_cnt = 0;

  for (;;)
  {
    if (osSemaphoreAcquire(RxPktSemaphore, TIME_WAITING_FOR_INPUT) == osOK)
    {
      g_rx_sem_cnt++;

      do
      {
        p = low_level_input(netif);
        if (p != NULL)
        {
          int arp_for_me = is_arp_for_me(p, netif);
          uint32_t tx_before = g_low_level_output_cnt;

          DebugUART_Print("[RX->INPUT] p=%p len=%u tot=%u ref=%u input_fn=%p arp_for_me=%d\r\n",
                          (void *)p,
                          (unsigned)p->len,
                          (unsigned)p->tot_len,
                          (unsigned)p->ref,
                          (void *)netif->input,
                          arp_for_me);

          /* Диагностика первых входящих пакетов */
          if (dbg_pkt_cnt < 20U)
          {
            dbg_pkt_cnt++;
            dump_arp_packet(p, netif);
          }

          err_t in_err = netif->input(p, netif);

          DebugUART_Print("[RX->INPUT] netif->input returned %d, tx_before=%lu tx_after=%lu\r\n",
                          (int)in_err,
                          (unsigned long)tx_before,
                          (unsigned long)g_low_level_output_cnt);

          if (arp_for_me)
          {
            DebugUART_Print("[RX->INPUT] ARP-for-me handled, TX delta=%ld\r\n",
                            (long)(g_low_level_output_cnt - tx_before));
          }

          if (in_err != ERR_OK)
          {
            DebugUART_Print("[RX->INPUT] input error, freeing pbuf\r\n");
            pbuf_free(p);
          }
        }
      } while (p != NULL);
    }
  }
}

#if !LWIP_ARP
/**
 * This function has to be completed by user in case of ARP OFF.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if ...
 */
static err_t low_level_output_arp_off(struct netif *netif, struct pbuf *q, const ip4_addr_t *ipaddr)
{
  err_t errval;
  errval = ERR_OK;

/* USER CODE BEGIN 5 */

/* USER CODE END 5 */

  return errval;

}
#endif /* LWIP_ARP */

/**
 * @brief Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 * This function should be passed as a parameter to netif_add().
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if the loopif is initialized
 *         ERR_MEM if private data couldn't be allocated
 *         any other err_t on error
 */
err_t ethernetif_init(struct netif *netif)
{
  LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
  netif->hostname = "lwip";
#endif

  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;

#if LWIP_IPV4
#if LWIP_ARP || LWIP_ETHERNET
#if LWIP_ARP
  netif->output = etharp_output;
#else
  netif->output = low_level_output_arp_off;
#endif
#endif
#endif

#if LWIP_IPV6
  netif->output_ip6 = ethip6_output;
#endif

  netif->linkoutput = low_level_output;

  DebugUART_Print("[ETHINIT] before low_level_init\r\n");
  DebugUART_Print("[ETHINIT] netif=%p input=%p output=%p linkoutput=%p flags=0x%02X\r\n",
                  (void *)netif,
                  (void *)netif->input,
                  (void *)netif->output,
                  (void *)netif->linkoutput,
                  (unsigned)netif->flags);

  low_level_init(netif);

  DebugUART_Print("[ETHINIT] after low_level_init\r\n");
  DebugUART_Print("[ETHINIT] netif=%p input=%p output=%p linkoutput=%p flags=0x%02X\r\n",
                  (void *)netif,
                  (void *)netif->input,
                  (void *)netif->output,
                  (void *)netif->linkoutput,
                  (unsigned)netif->flags);

#if LWIP_ARP
  DebugUART_Print("[ETHINIT] LWIP_ARP=1, output=etharp_output expected\r\n");
#else
  DebugUART_Print("[ETHINIT] WARNING: LWIP_ARP=0\r\n");
#endif

  DebugUART_Print("[ETHINIT] netif->input=%p netif->output=%p netif->linkoutput=%p flags=0x%02X\r\n",
                  (void *)netif->input,
                  (void *)netif->output,
                  (void *)netif->linkoutput,
                  (unsigned)netif->flags);

  return ERR_OK;
}

/**
  * @brief  Custom Rx pbuf free callback
  * @param  pbuf: pbuf to be freed
  * @retval None
  */
void pbuf_free_custom(struct pbuf *p)
{
  struct pbuf_custom *custom_pbuf = (struct pbuf_custom *)p;

  static uint32_t rx_free_cnt = 0;
  rx_free_cnt++;

  if (rx_free_cnt <= 40U)
  {
    DebugUART_Print("[RXFREE] cnt=%lu p=%p ref=%u len=%u tot_len=%u\r\n",
                    (unsigned long)rx_free_cnt,
                    (void *)p,
                    (unsigned)p->ref,
                    (unsigned)p->len,
                    (unsigned)p->tot_len);
  }

  LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);

  if (RxAllocStatus == RX_ALLOC_ERROR)
  {
    RxAllocStatus = RX_ALLOC_OK;
    osSemaphoreRelease(RxPktSemaphore);
  }
}
/* USER CODE BEGIN 6 */

/**
* @brief  Returns the current time in milliseconds
*         when LWIP_TIMERS == 1 and NO_SYS == 1
* @param  None
* @retval Current Time value
*/
u32_t sys_now(void)
{
  return HAL_GetTick();
}

/* USER CODE END 6 */

/**
  * @brief  Initializes the ETH MSP.
  * @param  ethHandle: ETH handle
  * @retval None
  */

void HAL_ETH_MspInit(ETH_HandleTypeDef* ethHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if(ethHandle->Instance==ETH)
  {
	  HAL_SYSCFG_ETHInterfaceSelect(SYSCFG_ETH_RMII);
	  DebugUART_Print("[ETH] SYSCFG->PMCR = 0x%08lX\r\n", (unsigned long)SYSCFG->PMCR);

    __HAL_RCC_ETH1MAC_CLK_ENABLE();
    __HAL_RCC_ETH1TX_CLK_ENABLE();
    __HAL_RCC_ETH1RX_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PC1 -> ETH_MDC, PC4 -> ETH_RXD0, PC5 -> ETH_RXD1 */
    GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* PA1 -> ETH_REF_CLK, PA2 -> ETH_MDIO, PA7 -> ETH_CRS_DV */
    GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* PB11 -> ETH_TX_EN, PB12 -> ETH_TXD0, PB13 -> ETH_TXD1 */
    GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    HAL_NVIC_SetPriority(ETH_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);
  }
}

void HAL_ETH_MspDeInit(ETH_HandleTypeDef* ethHandle)
{
  if(ethHandle->Instance==ETH)
  {
    __HAL_RCC_ETH1MAC_CLK_DISABLE();
    __HAL_RCC_ETH1TX_CLK_DISABLE();
    __HAL_RCC_ETH1RX_CLK_DISABLE();

    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13);

    HAL_NVIC_DisableIRQ(ETH_IRQn);
  }
}

/*******************************************************************************
                       PHI IO Functions
*******************************************************************************/
/**
  * @brief  Initializes the MDIO interface GPIO and clocks.
  * @param  None
  * @retval 0 if OK, -1 if ERROR
  */
int32_t ETH_PHY_IO_Init(void)
{
  /* We assume that MDIO GPIO configuration is already done
     in the ETH_MspInit() else it should be done here
  */

  /* Configure the MDIO Clock */
  HAL_ETH_SetMDIOClockRange(&heth);

  return 0;
}

/**
  * @brief  De-Initializes the MDIO interface .
  * @param  None
  * @retval 0 if OK, -1 if ERROR
  */
int32_t ETH_PHY_IO_DeInit (void)
{
  return 0;
}

/**
  * @brief  Read a PHY register through the MDIO interface.
  * @param  DevAddr: PHY port address
  * @param  RegAddr: PHY register address
  * @param  pRegVal: pointer to hold the register value
  * @retval 0 if OK -1 if Error
  */
int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal)
{
  if(HAL_ETH_ReadPHYRegister(&heth, DevAddr, RegAddr, pRegVal) != HAL_OK)
  {
    return -1;
  }

  return 0;
}

/**
  * @brief  Write a value to a PHY register through the MDIO interface.
  * @param  DevAddr: PHY port address
  * @param  RegAddr: PHY register address
  * @param  RegVal: Value to be written
  * @retval 0 if OK -1 if Error
  */
int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal)
{
  if(HAL_ETH_WritePHYRegister(&heth, DevAddr, RegAddr, RegVal) != HAL_OK)
  {
    return -1;
  }

  return 0;
}

/**
  * @brief  Get the time in millisecons used for internal PHY driver process.
  * @retval Time value
  */
int32_t ETH_PHY_IO_GetTick(void)
{
  return HAL_GetTick();
}

/**
  * @brief  Check the ETH link state then update ETH driver and netif link accordingly.
  * @retval None
  */
void ethernet_link_thread(void* argument)
{
  ETH_MACConfigTypeDef MACConf = {0};
  int32_t PHYLinkState = 0;
  struct netif *netif = (struct netif *)argument;

  for (;;)
  {
    PHYLinkState = LAN8742_GetLinkState(&LAN8742);

    if (PHYLinkState <= LAN8742_STATUS_LINK_DOWN)
    {
      if (netif_is_link_up(netif))
      {
        HAL_ETH_Stop_IT(&heth);

        DebugUART_Print("[ETH] link_thread: scheduling LINK DOWN in tcpip_thread\r\n");
        tcpip_callback(netif_link_down_in_tcpip, netif);
      }

      osDelay(100);
      continue;
    }

    uint32_t speed  = ETH_SPEED_100M;
    uint32_t duplex = ETH_FULLDUPLEX_MODE;

    switch (PHYLinkState)
    {
      case LAN8742_STATUS_100MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE;
        speed  = ETH_SPEED_100M;
        break;

      case LAN8742_STATUS_100MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE;
        speed  = ETH_SPEED_100M;
        break;

      case LAN8742_STATUS_10MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE;
        speed  = ETH_SPEED_10M;
        break;

      case LAN8742_STATUS_10MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE;
        speed  = ETH_SPEED_10M;
        break;

      default:
        break;
    }

    if (!netif_is_link_up(netif))
    {
      HAL_ETH_GetMACConfig(&heth, &MACConf);
      MACConf.DuplexMode = duplex;
      MACConf.Speed      = speed;
      HAL_ETH_SetMACConfig(&heth, &MACConf);

      if (HAL_ETH_Start_IT(&heth) != HAL_OK)
      {
        DebugUART_Print("[ETH] HAL_ETH_Start_IT failed\r\n");
      }
      else
      {
        DebugUART_Print("[ETH] HAL_ETH_Start_IT OK, speed=%s duplex=%s\r\n",
                        (speed == ETH_SPEED_100M) ? "100M" : "10M",
                        (duplex == ETH_FULLDUPLEX_MODE) ? "FULL" : "HALF");
      }

      DebugUART_Print("[ETH] link_thread: scheduling LINK UP in tcpip_thread\r\n");
      tcpip_callback(netif_link_up_in_tcpip, netif);
    }

    osDelay(100);
  }
}
/* USER CODE BEGIN ETH link Thread core code for User BSP */

/* USER CODE END ETH link Thread core code for User BSP */
void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
  struct pbuf_custom *pcustom = LWIP_MEMPOOL_ALLOC(RX_POOL);
  static uint32_t dbg_alloc_cnt = 0;

  if (pcustom != NULL)
  {
    struct pbuf *p = (struct pbuf *)pcustom;
    uint8_t *base = (uint8_t *)pcustom + offsetof(RxBuff_t, buff);

    /* DMA пишет в буфер со сдвигом на ETH_PAD_SIZE */
    *buff = base + ETH_PAD_SIZE;

    pcustom->custom_free_function = pbuf_free_custom;

    /* ВАЖНО:
       payload pbuf должен указывать на base,
       а DMA будет писать в base + ETH_PAD_SIZE.
    */
    pbuf_alloced_custom(PBUF_RAW,
                        0,
                        PBUF_REF,
                        pcustom,
                        base,
                        ETH_RX_BUFFER_SIZE + ETH_PAD_SIZE);

    if (dbg_alloc_cnt < 40U)
    {
      dbg_alloc_cnt++;
      DebugUART_Print("[RXALLOC] OK cnt=%lu pcustom=%p pbuf=%p buff=%p base=%p payload=%p\r\n",
                      (unsigned long)dbg_alloc_cnt,
                      (void *)pcustom,
                      (void *)p,
                      (void *)*buff,
                      (void *)base,
                      p->payload);
    }
  }
  else
  {
    RxAllocStatus = RX_ALLOC_ERROR;
    *buff = NULL;
    DebugUART_Print("[RXALLOC] FAIL: pool empty\r\n");
  }
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
  struct pbuf **ppStart = (struct pbuf **)pStart;
  struct pbuf **ppEnd   = (struct pbuf **)pEnd;
  struct pbuf *p;
  struct pbuf *q;

  p = (struct pbuf *)(buff - ETH_PAD_SIZE - offsetof(RxBuff_t, buff));

  p->next    = NULL;
  p->len     = Length;
  p->tot_len = Length;

  if (*ppStart == NULL)
  {
    *ppStart = p;
  }
  else
  {
    (*ppEnd)->next = p;

    for (q = *ppStart; q != p; q = q->next)
    {
      q->tot_len = (u16_t)(q->tot_len + Length);
    }
  }

  *ppEnd = p;

  invalidate_dcache_range(buff, Length);
}

void HAL_ETH_TxFreeCallback(uint32_t *buff)
{
  DebugUART_Print("[TX] HAL_ETH_TxFreeCallback ENTER buff=%p\r\n", (void *)buff);

  if (buff != NULL)
  {
    struct pbuf *p = (struct pbuf *)buff;

    DebugUART_Print("[TX] HAL_ETH_TxFreeCallback pbuf=%p ref_before=%u len=%u tot_len=%u\r\n",
                    (void *)p,
                    (unsigned)p->ref,
                    (unsigned)p->len,
                    (unsigned)p->tot_len);

    pbuf_free(p);

    DebugUART_Print("[TX] HAL_ETH_TxFreeCallback pbuf_free DONE\r\n");
  }

  DebugUART_Print("[TX] HAL_ETH_TxFreeCallback EXIT\r\n");
}
/* USER CODE BEGIN 8 */

/* USER CODE END 8 */


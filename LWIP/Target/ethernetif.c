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

#include "lwip/etharp.h"

extern struct netif gnetif;
void invalidate_dcache_range(void *addr, uint32_t size);
static uint8_t g_MACAddr[6] = {0x00,0x80,0xE1,0x00,0x00,0x00};

/* THIS IS THE DEFINITION (only once in the whole project) */
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

  /* ручной паддинг до ближайшей 32-байтной границы */
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

static err_t low_level_output(struct netif *netif, struct pbuf *p);
static void TryManualArpReply(const struct pbuf *p, struct netif *netif);

// глобальные счётчики
volatile uint32_t g_rx_irq_cnt = 0;
volatile uint32_t g_rx_sem_cnt = 0;

volatile uint32_t g_tx_cplt_cnt = 0;
volatile uint32_t g_tx_err_cnt  = 0;

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
  if (!p) return;

  uint8_t head[64];
  uint16_t need = (uint16_t)sizeof(head);

  if (p->tot_len < 14) return;

  uint16_t got = pbuf_copy_head(head, need, p);
  if (got < 14) return;

  uint16_t eth_type = (uint16_t)((head[12] << 8) | head[13]);

  /*
  DebugUART_Print("[RX] ETH dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X type=0x%04X tot=%u\r\n",
                  head[0], head[1], head[2], head[3], head[4], head[5],
                  head[6], head[7], head[8], head[9], head[10], head[11],
                  (unsigned)eth_type,
                  (unsigned)p->tot_len);
   */

  if (eth_type == ETHTYPE_ARP)
  {
    DebugUART_Print("[RX] ETH ARP tot=%u\r\n", (unsigned)p->tot_len);

    if (got >= 14 + 28)
    {
      const uint8_t *arp = &head[14];

      uint16_t htype = (uint16_t)((arp[0] << 8) | arp[1]);
      uint16_t ptype = (uint16_t)((arp[2] << 8) | arp[3]);
      uint8_t  hlen  = arp[4];
      uint8_t  plen  = arp[5];
      uint16_t oper  = (uint16_t)((arp[6] << 8) | arp[7]);

      DebugUART_Print("[RX] ARP htype=%u ptype=0x%04X hlen=%u plen=%u op=%u\r\n",
                      (unsigned)htype, (unsigned)ptype,
                      (unsigned)hlen, (unsigned)plen,
                      (unsigned)oper);

      DebugUART_Print("[RX] ARP sender MAC=%02X:%02X:%02X:%02X:%02X:%02X sender IP=%u.%u.%u.%u\r\n",
                      arp[8], arp[9], arp[10], arp[11], arp[12], arp[13],
                      arp[14], arp[15], arp[16], arp[17]);

      DebugUART_Print("[RX] ARP target MAC=%02X:%02X:%02X:%02X:%02X:%02X target IP=%u.%u.%u.%u\r\n",
                      arp[18], arp[19], arp[20], arp[21], arp[22], arp[23],
                      arp[24], arp[25], arp[26], arp[27]);
    }
    return;
  }

  if (eth_type == ETHTYPE_IP)
  {
    DebugUART_Print("[RX] ETH IPv4 tot=%u\r\n", (unsigned)p->tot_len);

    if (got >= 14 + 20)
    {
      const uint8_t *ip = &head[14];
      uint8_t proto = ip[9];

      DebugUART_Print("[RX] IPv4 proto=%u src=%u.%u.%u.%u dst=%u.%u.%u.%u\r\n",
                      (unsigned)proto,
                      (unsigned)ip[12], (unsigned)ip[13], (unsigned)ip[14], (unsigned)ip[15],
                      (unsigned)ip[16], (unsigned)ip[17], (unsigned)ip[18], (unsigned)ip[19]);

      if (proto == 1 && got >= 14 + 20 + 8)
      {
        const uint8_t *icmp = &head[14 + 20];
        DebugUART_Print("[RX] ICMP type=%u code=%u\r\n",
                        (unsigned)icmp[0], (unsigned)icmp[1]);
      }
      else if (proto == 6 && got >= 14 + 20 + 20)
      {
        const uint8_t *tcp = &head[14 + 20];
        uint16_t sport = (uint16_t)((tcp[0] << 8) | tcp[1]);
        uint16_t dport = (uint16_t)((tcp[2] << 8) | tcp[3]);
        uint8_t flags = tcp[13];

        DebugUART_Print("[RX] TCP sport=%u dport=%u flags=0x%02X\r\n",
                        (unsigned)sport, (unsigned)dport, (unsigned)flags);
      }
    }
    return;
  }
}

static void TryManualArpReply(const struct pbuf *p, struct netif *netif)
{
  DebugUART_Print("[ARP-MANUAL] enter p=%p netif=%p tot=%u\r\n",
                  (void *)p, (void *)netif, (unsigned)(p ? p->tot_len : 0));

  if ((p == NULL) || (netif == NULL))
  {
    DebugUART_Print("[ARP-MANUAL] exit: null arg\r\n");
    return;
  }

  if (p->tot_len < 42)
  {
    DebugUART_Print("[ARP-MANUAL] exit: too short (%u)\r\n", (unsigned)p->tot_len);
    return;
  }

  uint8_t head[64];
  u16_t copied = pbuf_copy_partial((struct pbuf *)p, head, sizeof(head), 0);
  if (copied < 42)
  {
    DebugUART_Print("[ARP-MANUAL] exit: copied=%u < 42\r\n", (unsigned)copied);
    return;
  }

  uint16_t eth_type = (uint16_t)((head[12] << 8) | head[13]);
  if (eth_type != 0x0806)
  {
    return; /* not ARP */
  }

  const uint8_t *arp = &head[14];

  uint16_t htype = (uint16_t)((arp[0] << 8) | arp[1]);
  uint16_t ptype = (uint16_t)((arp[2] << 8) | arp[3]);
  uint8_t  hlen  = arp[4];
  uint8_t  plen  = arp[5];
  uint16_t oper  = (uint16_t)((arp[6] << 8) | arp[7]);

  DebugUART_Print("[ARP-MANUAL] ARP htype=%u ptype=0x%04X hlen=%u plen=%u op=%u\r\n",
                  (unsigned)htype, (unsigned)ptype,
                  (unsigned)hlen, (unsigned)plen,
                  (unsigned)oper);

  if (htype != 1 || ptype != 0x0800 || hlen != 6 || plen != 4 || oper != 1)
  {
    DebugUART_Print("[ARP-MANUAL] exit: not Ethernet/IPv4 ARP request\r\n");
    return;
  }

  const uint8_t *sender_mac = &arp[8];
  const uint8_t *sender_ip  = &arp[14];
  const uint8_t *target_ip  = &arp[24];

  const ip4_addr_t *my_ip = netif_ip4_addr(netif);

  uint8_t my_ip_bytes[4] = {
    ip4_addr1(my_ip),
    ip4_addr2(my_ip),
    ip4_addr3(my_ip),
    ip4_addr4(my_ip)
  };

  DebugUART_Print("[ARP-MANUAL] sender=%u.%u.%u.%u target=%u.%u.%u.%u my=%u.%u.%u.%u\r\n",
                  sender_ip[0], sender_ip[1], sender_ip[2], sender_ip[3],
                  target_ip[0], target_ip[1], target_ip[2], target_ip[3],
                  my_ip_bytes[0], my_ip_bytes[1], my_ip_bytes[2], my_ip_bytes[3]);

  if (memcmp(target_ip, my_ip_bytes, 4) != 0)
  {
    DebugUART_Print("[ARP-MANUAL] exit: target IP is not mine\r\n");
    return;
  }

  DebugUART_Print("[ARP-MANUAL] building ARP reply\r\n");

  /* Ethernet minimum frame size without FCS = 60 bytes */
  const uint16_t arp_frame_len = 60;

  struct pbuf *tx = pbuf_alloc(PBUF_RAW, arp_frame_len + ETH_PAD_SIZE, PBUF_RAM);
  if (tx == NULL)
  {
    DebugUART_Print("[ARP-MANUAL] exit: pbuf_alloc failed\r\n");
    return;
  }

  uint8_t *base = (uint8_t *)tx->payload;
  memset(base, 0, arp_frame_len + ETH_PAD_SIZE);

  /* Reserve pad bytes because low_level_output() does pbuf_header(p, -ETH_PAD_SIZE) */
  uint8_t *f = base + ETH_PAD_SIZE;

  /* Ethernet header */
  memcpy(&f[0],  sender_mac, 6);   /* dst */
  memcpy(&f[6],  g_MACAddr,  6);   /* src */
  f[12] = 0x08;
  f[13] = 0x06;                    /* ARP */

  /* ARP payload */
  f[14] = 0x00; f[15] = 0x01;   /* htype = Ethernet */
  f[16] = 0x08; f[17] = 0x00;   /* ptype = IPv4 */
  f[18] = 0x06;                 /* hlen = 6 */
  f[19] = 0x04;                 /* plen = 4 */
  f[20] = 0x00; f[21] = 0x02;   /* oper = reply */

  memcpy(&f[22], g_MACAddr,   6); /* sender MAC = us */
  memcpy(&f[28], my_ip_bytes, 4); /* sender IP  = us */
  memcpy(&f[32], sender_mac,  6); /* target MAC = original sender */
  memcpy(&f[38], sender_ip,   4); /* target IP  = original sender */

  /* bytes 42..59 stay zero as Ethernet padding */

  DebugUART_Print("[ARP-MANUAL] TX frame bytes (first 60):\r\n");
  for (int i = 0; i < arp_frame_len; i++)
  {
    DebugUART_Print("%02X ", f[i]);
    if (((i + 1) % 16) == 0)
      DebugUART_Print("\r\n");
  }
  DebugUART_Print("\r\n");

  err_t e = low_level_output(netif, tx);
  DebugUART_Print("[ARP-MANUAL] reply send err=%d\r\n", (int)e);

  pbuf_free(tx);
}

static void netif_link_up_in_tcpip(void *arg)
{
  struct netif *netif = (struct netif *)arg;

  netif_set_link_up(netif);
  netif_set_up(netif);

  DebugUART_Print("[ETH] tcpip: netif link UP + netif UP\r\n");

  etharp_gratuitous(netif);
  DebugUART_Print("[ETH] tcpip: gratuitous ARP requested\r\n");

  if (g_ethLinkEvt)
  {
    osEventFlagsSet(g_ethLinkEvt, APP_ETH_EVT_LINK_UP);
  }
}

static void netif_link_down_in_tcpip(void *arg)
{
  struct netif *netif = (struct netif *)arg;

  netif_set_link_down(netif);
  netif_set_down(netif);

  DebugUART_Print("[ETH] tcpip: netif link DOWN + netif DOWN\r\n");

  if (g_ethLinkEvt)
  {
    osEventFlagsClear(g_ethLinkEvt, APP_ETH_EVT_LINK_UP);
  }
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
  (void)handlerEth;
  g_tx_cplt_cnt++;
  DebugUART_Print("[TX] HAL_ETH_TxCpltCallback cnt=%lu\r\n", (unsigned long)g_tx_cplt_cnt);
  osSemaphoreRelease(TxPktSemaphore);
}
/**
  * @brief  Ethernet DMA transfer error callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *handlerEth)
{
  g_tx_err_cnt++;
  DebugUART_Print("[ETH] ERROR callback cnt=%lu hal_err=0x%08lX dma_err=0x%08lX\r\n",
                  (unsigned long)g_tx_err_cnt,
                  (unsigned long)HAL_ETH_GetError(handlerEth),
                  (unsigned long)HAL_ETH_GetDMAError(handlerEth));

  if((HAL_ETH_GetDMAError(handlerEth) & ETH_DMACSR_RBU) == ETH_DMACSR_RBU)
  {
     osSemaphoreRelease(RxPktSemaphore);
  }
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
  attributes.priority = osPriorityRealtime;
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

  /* initial link state */
  PHYLinkState = LAN8742_GetLinkState(&LAN8742);

  if (PHYLinkState <= LAN8742_STATUS_LINK_DOWN)
  {
    netif_set_link_down(netif);
    LWIP_PLATFORM_DIAG(("[ETH] initial PHY link DOWN\r\n"));
    /* флаг события НЕ ставим, EthTask будет ждать реального UP */
    return;
  }

  switch (PHYLinkState)
  {
    case LAN8742_STATUS_100MBITS_FULLDUPLEX: duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_100M; break;
    case LAN8742_STATUS_100MBITS_HALFDUPLEX: duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_100M; break;
    case LAN8742_STATUS_10MBITS_FULLDUPLEX:  duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_10M;  break;
    case LAN8742_STATUS_10MBITS_HALFDUPLEX:  duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_10M;  break;
    default:                                duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_100M; break;
  }

  HAL_ETH_GetMACConfig(&heth, &MACConf);
  MACConf.DuplexMode = duplex;
  MACConf.Speed      = speed;
  HAL_ETH_SetMACConfig(&heth, &MACConf);

  HAL_ETH_Start_IT(&heth);

  DebugUART_Print("[ETH] low_level_init: scheduling initial LINK UP in tcpip_thread\r\n");
  tcpip_callback(netif_link_up_in_tcpip, netif);

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

  uint32_t i = 0U;
  struct pbuf *q = NULL;
  err_t errval = ERR_OK;
  ETH_BufferTypeDef Txbuffer[ETH_TX_DESC_CNT];

  LWIP_UNUSED_ARG(netif);

  memset(Txbuffer, 0, sizeof(Txbuffer));

  uint16_t eth_type = 0xFFFF;
  if (p != NULL && p->tot_len >= 14)
  {
    uint8_t hdr[14];
    pbuf_copy_partial(p, hdr, sizeof(hdr), 0);
    eth_type = (uint16_t)((hdr[12] << 8) | hdr[13]);
  }

  DebugUART_Print("[TX] low_level_output enter: tot_len=%u eth_type=0x%04X\r\n",
                  (unsigned)(p ? p->tot_len : 0),
                  (unsigned)eth_type);

  for (q = p; q != NULL; q = q->next)
  {
    if (i >= ETH_TX_DESC_CNT)
    {
      DebugUART_Print("[TX] ERR: too many pbufs in chain (%lu)\r\n", (unsigned long)i);
#if ETH_PAD_SIZE
      pbuf_header(p, ETH_PAD_SIZE);
#endif
      return ERR_IF;
    }

    Txbuffer[i].buffer = (uint8_t*)q->payload;
    Txbuffer[i].len    = q->len;
    Txbuffer[i].next   = NULL;

    /*
    DebugUART_Print("[TX]   seg[%lu] buf=%p len=%u\r\n",
                    (unsigned long)i,
                    (void*)Txbuffer[i].buffer,
                    (unsigned)Txbuffer[i].len);
     */
    if (i > 0U)
      Txbuffer[i - 1U].next = &Txbuffer[i];

    i++;
  }

  for (uint32_t j = 0; j < i; j++)
  {
    clean_dcache_range(Txbuffer[j].buffer, Txbuffer[j].len);
  }

  TxConfig.Length   = p->tot_len;
  TxConfig.TxBuffer = Txbuffer;
  TxConfig.pData    = p;

  /* IMPORTANT:
   * checksum offload only for IP/IPv6 packets.
   * For ARP and other non-IP Ethernet frames it must be disabled.
   */
  if ((eth_type == 0x0800) || (eth_type == 0x86DD))
  {
    TxConfig.Attributes   = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
    TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
  }
  else
  {
    TxConfig.Attributes   = ETH_TX_PACKETS_FEATURES_CRCPAD;
    TxConfig.ChecksumCtrl = ETH_CHECKSUM_DISABLE;
  }

  DebugUART_Print("[TX] cfg: eth_type=0x%04X attr=0x%08lX csum=0x%08lX\r\n",
                  (unsigned)eth_type,
                  (unsigned long)TxConfig.Attributes,
                  (unsigned long)TxConfig.ChecksumCtrl);

  pbuf_ref(p);

  do
  {
    HAL_StatusTypeDef st = HAL_ETH_Transmit_IT(&heth, &TxConfig);

    if (st == HAL_OK)
    {
      DebugUART_Print("[TX] HAL_ETH_Transmit_IT -> HAL_OK\r\n");
      errval = ERR_OK;
    }
    else
    {
      uint32_t hal_err = HAL_ETH_GetError(&heth);

      DebugUART_Print("[TX] HAL_ETH_Transmit_IT FAIL st=%d hal_err=0x%08lX dma_err=0x%08lX\r\n",
                      (int)st,
                      (unsigned long)hal_err,
                      (unsigned long)HAL_ETH_GetDMAError(&heth));

      if (hal_err & HAL_ETH_ERROR_BUSY)
      {
        DebugUART_Print("[TX] BUSY -> wait TxPktSemaphore\r\n");
        osSemaphoreAcquire(TxPktSemaphore, ETHIF_TX_TIMEOUT);
        HAL_ETH_ReleaseTxPacket(&heth);
        errval = ERR_BUF;
      }
      else
      {
        DebugUART_Print("[TX] FATAL TX ERROR\r\n");
        pbuf_free(p);
        errval = ERR_IF;
      }
    }
  } while (errval == ERR_BUF);

#if ETH_PAD_SIZE
  pbuf_header(p, ETH_PAD_SIZE);
#endif

  DebugUART_Print("[TX] low_level_output exit err=%d\r\n", (int)errval);
  return errval;
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
    /* HAL_ETH_ReadData вернёт указатель на pbuf-chain (zero-copy) */
    HAL_ETH_ReadData(&heth, (void **)&p);

    if (p != NULL)
    {
      /* Логируем факт RX (ARP/IP/прочее) */
       log_rx_pbuf(p);
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
  struct netif *netif = (struct netif *) argument;

  for( ;; )
  {
	  if (osSemaphoreAcquire(RxPktSemaphore, TIME_WAITING_FOR_INPUT) == osOK)
	  {
	    g_rx_sem_cnt++;

	    do
	    {
	      p = low_level_input(netif);
	      if (p != NULL)
	      {
	          /* Temporary workaround: reply to ARP requests manually */
	          TryManualArpReply(p, netif);

	          err_t in_err = netif->input(p, netif);
	          if (in_err != ERR_OK)
	          {
	            DebugUART_Print("[RX->LWIP] netif->input err=%d\r\n", (int)in_err);
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
  /* Initialize interface hostname */
  netif->hostname = "lwip";
#endif /* LWIP_NETIF_HOSTNAME */

  /*
   * Initialize the snmp variables and counters inside the struct netif.
   * The last argument should be replaced with your link speed, in units
   * of bits per second.
   */
  // MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, LINK_SPEED_OF_YOUR_NETIF_IN_BPS);

  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;
  /* We directly use etharp_output() here to save a function call.
   * You can instead declare your own function an call etharp_output()
   * from it if you have to do some checks before sending (e.g. if link
   * is available...) */

#if LWIP_IPV4
#if LWIP_ARP || LWIP_ETHERNET
#if LWIP_ARP
  netif->output = etharp_output;
#else
  /* The user should write its own code in low_level_output_arp_off function */
  netif->output = low_level_output_arp_off;
#endif /* LWIP_ARP */
#endif /* LWIP_ARP || LWIP_ETHERNET */
#endif /* LWIP_IPV4 */

#if LWIP_IPV6
  netif->output_ip6 = ethip6_output;
#endif /* LWIP_IPV6 */

  netif->linkoutput = low_level_output;

  /* initialize the hardware */
  low_level_init(netif);

  return ERR_OK;
}

/**
  * @brief  Custom Rx pbuf free callback
  * @param  pbuf: pbuf to be freed
  * @retval None
  */
void pbuf_free_custom(struct pbuf *p)
{
  struct pbuf_custom* custom_pbuf = (struct pbuf_custom*)p;
  LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);

  /* If the Rx Buffer Pool was exhausted, signal the ethernetif_input task to
   * call HAL_ETH_GetRxDataBuffer to rebuild the Rx descriptors. */

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
    __HAL_RCC_ETH1MAC_CLK_ENABLE();
    __HAL_RCC_ETH1TX_CLK_ENABLE();
    __HAL_RCC_ETH1RX_CLK_ENABLE();

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /** ETH GPIO Configuration
    PC1  ------> ETH_MDC
    PA1  ------> ETH_REF_CLK
    PA2  ------> ETH_MDIO
    PA7  ------> ETH_CRS_DV
    PC4  ------> ETH_RXD0
    PC5  ------> ETH_RXD1
    PB11 ------> ETH_TX_EN
    PB12 ------> ETH_TXD0
    PB13 ------> ETH_TXD1
    */

    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;

    GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

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
  /* USER CODE BEGIN ETH_MspDeInit 0 */

  /* USER CODE END ETH_MspDeInit 0 */
    /* Disable Peripheral clock */
    __HAL_RCC_ETH1MAC_CLK_DISABLE();
    __HAL_RCC_ETH1TX_CLK_DISABLE();
    __HAL_RCC_ETH1RX_CLK_DISABLE();

    /**ETH GPIO Configuration
    PC1     ------> ETH_MDC
    PA1     ------> ETH_REF_CLK
    PA2     ------> ETH_MDIO
    PA7     ------> ETH_CRS_DV
    PC4     ------> ETH_RXD0
    PC5     ------> ETH_RXD1
    PB11     ------> ETH_TX_EN
    PB12     ------> ETH_TXD0
    PB13     ------> ETH_TXD1
    */
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5);

    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_7);

    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_11|GPIO_PIN_12|GPIO_PIN_13);

    /* Peripheral interrupt Deinit*/
    HAL_NVIC_DisableIRQ(ETH_IRQn);

  /* USER CODE BEGIN ETH_MspDeInit 1 */

  /* USER CODE END ETH_MspDeInit 1 */
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
    	  //netif_set_link_down(netif);
    	  //netif_set_down(netif);

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
      case LAN8742_STATUS_100MBITS_FULLDUPLEX: duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_100M; break;
      case LAN8742_STATUS_100MBITS_HALFDUPLEX: duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_100M; break;
      case LAN8742_STATUS_10MBITS_FULLDUPLEX:  duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_10M;  break;
      case LAN8742_STATUS_10MBITS_HALFDUPLEX:  duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_10M;  break;
      default: break;
    }

    if (!netif_is_link_up(netif))
    {
      HAL_ETH_GetMACConfig(&heth, &MACConf);
      MACConf.DuplexMode = duplex;
      MACConf.Speed      = speed;
      HAL_ETH_SetMACConfig(&heth, &MACConf);

      HAL_ETH_Start_IT(&heth);
      //netif_set_link_up(netif);
      //netif_set_up(netif);

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
  struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
  static uint32_t dbg_alloc_cnt = 0;  //просто счетчик для вывода

  if (p)
  {
    uint8_t *base = (uint8_t *)p + offsetof(RxBuff_t, buff);

    /* DMA пишет кадр с отступом 2 байта, чтобы lwIP потом работал с выровненным payload */
    *buff = base + ETH_PAD_SIZE;

    /*

    if (dbg_alloc_cnt < 8)
    {
          DebugUART_Print("[RXALLOC] p=%p buff=%p off=%lu\r\n",
                          (void*)p,
                          (void*)*buff,
                          (unsigned long)offsetof(RxBuff_t, buff));
          dbg_alloc_cnt++;
     }
    */

    p->custom_free_function = pbuf_free_custom;
    pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUFFER_SIZE);
  }
  else
  {
    RxAllocStatus = RX_ALLOC_ERROR;
    *buff = NULL;
  }
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
  struct pbuf **ppStart = (struct pbuf **)pStart;
  struct pbuf **ppEnd   = (struct pbuf **)pEnd;
  struct pbuf *p = NULL;

  p = (struct pbuf *)(buff - ETH_PAD_SIZE - offsetof(RxBuff_t, buff));

  p->next    = NULL;
  p->tot_len = 0;
  p->len     = Length;

  if (!*ppStart)
  {
    *ppStart = p;
  }
  else
  {
    (*ppEnd)->next = p;
  }
  *ppEnd = p;

  for (p = *ppStart; p != NULL; p = p->next)
  {
    p->tot_len += Length;
  }

  invalidate_dcache_range(buff, Length);
}

void HAL_ETH_TxFreeCallback(uint32_t * buff)
{
/* USER CODE BEGIN HAL ETH TxFreeCallback */

  pbuf_free((struct pbuf *)buff);

/* USER CODE END HAL ETH TxFreeCallback */
}

/* USER CODE BEGIN 8 */

/* USER CODE END 8 */


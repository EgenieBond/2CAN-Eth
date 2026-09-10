/*
 * eth_raw_test.c
 *
 *  Created on: Aug 8, 2026
 *      Author: Egenie
 */

/*
 * eth_raw_test.c
 *
 * Диагностический тест:
 * отправка "сырых" Ethernet-кадров напрямую через low_level_output(),
 * в обход TCP/UDP/IP полностью. Цель — увидеть реальную интенсивность
 * передачи на нижнем уровне, без влияния логики протоколов верхнего
 * уровня (ретрансмиссии TCP, обработка UDP в стеке и т.д.).
 *
 * Кадры не предназначены для приёма и обработки платой или ПК —
 * их нужно поймать анализатором Wireshark на стороне ПК.
 */

#include "eth_raw_test.h"
#include "main.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "cmsis_os.h"
#include "debug_uart.h"
#include "eth_events.h"
#include <string.h>

extern struct netif gnetif;
extern void ETH_DebugPrintCounters(const char *tag);

/* ===== Параметры теста ===== */
#define RAW_ETH_HDR_LEN      14U
#define RAW_PAYLOAD_LEN      1024U
#define RAW_FRAME_LEN         (RAW_ETH_HDR_LEN + RAW_PAYLOAD_LEN)

/* Не IP (0x0800) и не ARP (0x0806) — экспериментальный EtherType,
   чтобы Windows на ПК не пытался разобрать кадр как сетевой пакет
   и не слал в ответ ICMP/что-либо ещё. 0x88B5 официально зарезервирован
   IEEE как "Local Experimental Ethertype 1". */
#define RAW_ETHERTYPE_HI      0x88U
#define RAW_ETHERTYPE_LO      0xB5U

#define RAW_TEST_FRAMES_NOPAUSE   50000U
#define RAW_TEST_FRAMES_PAUSE     3000U
#define RAW_TEST_PAUSE_MS         1U

/**
 * @brief Формирует один кадр и отправляет его напрямую в Ethernet,
 *        в обход TCP/UDP/IP.
 */
static err_t RawEthTest_SendFrame(uint32_t counter)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, ETH_PAD_SIZE + RAW_FRAME_LEN, PBUF_RAM);
    if (p == NULL)
    {
        return ERR_MEM;
    }

    uint8_t *buf = (uint8_t *)p->payload;
    uint8_t *eth = buf + ETH_PAD_SIZE;

    /* MAC назначения: broadcast — поймать сможет любой хост в сети,
       без необходимости знать точный MAC приёмника заранее. */
    memset(&eth[0], 0xFF, 6);

    /* MAC источника: наш собственный (плата) */
    memcpy(&eth[6], gnetif.hwaddr, 6);

    /* EtherType — экспериментальный, не IP/ARP */
    eth[12] = RAW_ETHERTYPE_HI;
    eth[13] = RAW_ETHERTYPE_LO;

    /* Полезная нагрузка: первые 4 байта — счётчик кадра (удобно видеть
       в Wireshark, не потерялись ли кадры), остальное — заполнение
       узнаваемым паттерном. */
    eth[14] = (uint8_t)(counter >> 24);
    eth[15] = (uint8_t)(counter >> 16);
    eth[16] = (uint8_t)(counter >> 8);
    eth[17] = (uint8_t)(counter);

    for (uint32_t i = 18U; i < RAW_FRAME_LEN; i++)
    {
        eth[i] = (uint8_t)(i & 0xFFU);
    }

    /* Прямой вызов low_level_output(), минуя весь стек TCP/IP */
    err_t result = gnetif.linkoutput(&gnetif, p);

    pbuf_free(p);

    return result;
}

/**
 * @brief Прогоняет один тест: N кадров, с указанной паузой между ними.
 */
static void RawEthTest_RunBatch(const char *tag, uint32_t frame_count, uint32_t pause_ms)
{
    uint32_t sent_ok      = 0;
    uint32_t sent_fail    = 0;
    uint32_t print_step   = frame_count / 10U;
    uint32_t start_tick;

    if (print_step == 0U) { print_step = 1U; }

    DebugUART_Print("[RAW-ETH] === %s START === frames=%lu payload=%u pause_ms=%lu\r\n",
                    tag,
                    (unsigned long)frame_count,
                    (unsigned)RAW_PAYLOAD_LEN,
                    (unsigned long)pause_ms);

    start_tick = osKernelGetTickCount();

    for (uint32_t i = 0; i < frame_count; i++)
    {
        err_t err = RawEthTest_SendFrame(i);

        if (err == ERR_OK) { sent_ok++; }
        else                { sent_fail++; }

        if (pause_ms > 0U)
        {
            osDelay(pause_ms);
        }

        if ((i % print_step) == 0U)
        {
            DebugUART_Print("[RAW-ETH] %s progress: %lu/%lu ok=%lu fail=%lu\r\n",
                            tag,
                            (unsigned long)i,
                            (unsigned long)frame_count,
                            (unsigned long)sent_ok,
                            (unsigned long)sent_fail);
        }
    }

    {
        uint32_t elapsed_ms  = osKernelGetTickCount() - start_tick;
        uint64_t total_bytes = (uint64_t)sent_ok * RAW_FRAME_LEN;
        uint32_t speed_kbps  = 0U;

        if (elapsed_ms > 0U)
        {
            speed_kbps = (uint32_t)((total_bytes * 8ULL) / elapsed_ms);
        }

        DebugUART_Print("[RAW-ETH] === %s DONE ===\r\n", tag);
        DebugUART_Print("[RAW-ETH] frames sent OK : %lu\r\n", (unsigned long)sent_ok);
        DebugUART_Print("[RAW-ETH] frames failed  : %lu\r\n", (unsigned long)sent_fail);
        DebugUART_Print("[RAW-ETH] elapsed        : %lu ms\r\n", (unsigned long)elapsed_ms);
        DebugUART_Print("[RAW-ETH] total bytes    : %lu\r\n", (unsigned long)(uint32_t)total_bytes);
        DebugUART_Print("[RAW-ETH] avg speed      : %lu Kbit/s (%lu Mbit/s)\r\n",
                        (unsigned long)speed_kbps,
                        (unsigned long)(speed_kbps / 1000UL));
    }

    ETH_DebugPrintCounters(tag);
}

/**
 * @brief Задача теста: дожидается поднятия линка, потом прогоняет
 *        два теста подряд — без пауз и с паузой.
 */
static void EthRawTest_Task(void *argument)
{
    (void)argument;

    DebugUART_Print("[RAW-ETH] test task started, waiting link up...\r\n");

    if (g_ethLinkEvt != NULL)
    {
        osEventFlagsWait(g_ethLinkEvt, APP_ETH_EVT_LINK_UP, osFlagsWaitAny, osWaitForever);
    }
    else
    {
        while (!netif_is_link_up(&gnetif))
        {
            osDelay(50);
        }
    }

    while (!netif_is_up(&gnetif) || !netif_is_link_up(&gnetif))
    {
        osDelay(50);
    }

    DebugUART_Print("[RAW-ETH] link up. CPU = %lu MHz\r\n",
                    (unsigned long)(SystemCoreClock / 1000000UL));

    osDelay(2000);

    /* ===== ТЕСТ 1: без пауз — максимальная скорость ===== */
    RawEthTest_RunBatch("NO-PAUSE", RAW_TEST_FRAMES_NOPAUSE, 0U);

    osDelay(3000);

    /* ===== ТЕСТ 2: с паузой 1 мс между кадрами ===== */
    RawEthTest_RunBatch("PAUSE-1MS", RAW_TEST_FRAMES_PAUSE, RAW_TEST_PAUSE_MS);

    DebugUART_Print("[RAW-ETH] ALL TESTS DONE\r\n");

    for (;;)
    {
        osDelay(10000);
    }
}

void EthRawTest_Start(void)
{
    const osThreadAttr_t attr = {
        .name       = "RawEthTest",
        .stack_size = 4096,
        .priority   = (osPriority_t)osPriorityAboveNormal
    };

    if (osThreadNew(EthRawTest_Task, NULL, &attr) == NULL)
    {
        DebugUART_Print("[RAW-ETH] ERROR: task creation failed\r\n");
    }
    else
    {
        DebugUART_Print("[RAW-ETH] task created\r\n");
    }
}

/*
 * raw_tcp_server.c
 *
 * TCP RAW server (single client)
 */

#include "raw_tcp_server.h"
#include "client_handler.h"

#include "lwip/tcp.h"
#include "lwip/inet.h"
#include "lwip/tcpip.h"

#include "cmsis_os.h"
#include "debug_uart.h"

#include <string.h>
#include <stdint.h>

#define TCP_SERVER_PORT 2001

/*
 * ETH_BENCHMARK_MODE:
 *   0 = обычный рабочий режим (SLCAN)
 *   1 = Тест 1: ПК -> плата. Плата только считает входящие байты.
 *   2 = Тест 2: плата -> ПК. Плата сама генерирует и льёт данные клиенту.
 */
#define ETH_BENCHMARK_MODE 1

#define RAW_TCP_TX_RING_SIZE 8192

static struct tcp_pcb *server_pcb = NULL;
static struct tcp_pcb *client_pcb = NULL;

static uint8_t g_tx_ring[RAW_TCP_TX_RING_SIZE];
static volatile uint32_t g_tx_head = 0;
static volatile uint32_t g_tx_tail = 0;
static volatile uint8_t g_tx_flush_scheduled = 0;

static uint32_t g_tx_drop_count = 0;
static uint32_t g_tcp_err_mem_count = 0;

/* ===== Benchmark state ===== */
#if (ETH_BENCHMARK_MODE == 1) || (ETH_BENCHMARK_MODE == 2)

#define BENCH_TARGET_BYTES  (100UL * 1024UL * 1024UL)   /* 100 МБ */
#define BENCH_PRINT_STEP    (10UL  * 1024UL * 1024UL)   /* прогресс каждые 10 МБ */

static uint32_t g_bench_bytes      = 0;
static uint32_t g_bench_last_print = 0;
static uint32_t g_bench_start_tick = 0;
static uint8_t  g_bench_started    = 0;
static uint8_t  g_bench_done       = 0;
static uint32_t g_bench_start_tick_abs = 0;

#include "lwip/stats.h"

static void Bench_PrintProgress(void)
{
    uint32_t elapsed_ms = osKernelGetTickCount() - g_bench_start_tick;
    uint32_t speed_kbps = 0;
    u16_t rcv_wnd = 0;
    u32_t rcv_nxt = 0;

    if (elapsed_ms > 0)
    {
        speed_kbps = (uint32_t)((uint64_t)g_bench_bytes * 8ULL / elapsed_ms);
    }

    if (client_pcb != NULL)
    {
        rcv_wnd = client_pcb->rcv_wnd;
        rcv_nxt = client_pcb->rcv_nxt;
    }

    DebugUART_Print("[BENCH] %lu MB | %lu ms | %lu Kbit/s (%lu Mbit/s) | rcv_wnd=%u rcv_nxt=%lu tcp_seg err=%u pbuf_pool err=%u\r\n",
                    (unsigned long)(g_bench_bytes / (1024UL * 1024UL)),
                    (unsigned long)elapsed_ms,
                    (unsigned long)speed_kbps,
                    (unsigned long)(speed_kbps / 1000UL),
                    (unsigned)rcv_wnd,
                    (unsigned long)rcv_nxt,
                    (unsigned)lwip_stats.memp[MEMP_TCP_SEG]->err,
                    (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->err);
}

static void Bench_Reset(void)
{
    g_bench_bytes      = 0;
    g_bench_last_print = 0;
    g_bench_start_tick = 0;
    g_bench_start_tick_abs = 0;
    g_bench_started    = 0;
    g_bench_done       = 0;
}

#endif /* ETH_BENCHMARK_MODE 1 or 2 */

/* ===== Тест 2: генератор данных плата -> ПК ===== */
#if (ETH_BENCHMARK_MODE == 2)

/*
 * Буфер с фиксированными данными для отправки.
 * Плата льёт его повторно пока не дойдёт до BENCH_TARGET_BYTES.
 */
#define BENCH_TX_BUF_SIZE   1460U   /* один TCP сегмент */

static uint8_t g_bench_tx_buf[BENCH_TX_BUF_SIZE];
static uint8_t g_bench_tx_buf_ready = 0;

static void Bench_TxInit(void)
{
    /* заполняем буфер один раз фиксированным паттерном */
    for (uint32_t i = 0; i < BENCH_TX_BUF_SIZE; i++)
    {
        g_bench_tx_buf[i] = (uint8_t)(i & 0xFFU);
    }
    g_bench_tx_buf_ready = 1;
}

/*
 * Вызывается из tcpip_thread — льём данные пока есть место в send buffer.
 */
static void Bench_TxPump(void)
{
    if (client_pcb == NULL)    { DebugUART_Print("[BENCH TX] pump: no client\r\n"); return; }
    if (!g_bench_tx_buf_ready) { DebugUART_Print("[BENCH TX] pump: buf not ready\r\n"); return; }
    if (g_bench_done)          { return; }

    uint32_t iterations = 0;

    for (;;)
    {
        if (g_bench_bytes >= BENCH_TARGET_BYTES)
        {
            if (!g_bench_done)
            {
                g_bench_done = 1;
                Bench_PrintProgress();
                DebugUART_Print("[BENCH TX] === DONE ===\r\n");
            }
            break;
        }

        u16_t sndbuf = tcp_sndbuf(client_pcb);

        if (iterations == 0)
        {
            DebugUART_Print("[BENCH TX] pump enter: sndbuf=%u bytes_sent=%lu\r\n",
                            (unsigned)sndbuf, (unsigned long)g_bench_bytes);
        }

        if (sndbuf < BENCH_TX_BUF_SIZE)
        {
            DebugUART_Print("[BENCH TX] sndbuf too small=%u, waiting sent cb\r\n",
                            (unsigned)sndbuf);
            break;
        }

        uint32_t remaining = BENCH_TARGET_BYTES - g_bench_bytes;
        uint16_t to_send   = (remaining >= BENCH_TX_BUF_SIZE)
                             ? BENCH_TX_BUF_SIZE
                             : (uint16_t)remaining;

        err_t wr = tcp_write(client_pcb, g_bench_tx_buf, to_send, TCP_WRITE_FLAG_COPY);

                if (wr == ERR_MEM) { break; }
                if (wr != ERR_OK)  { DebugUART_Print("[BENCH TX] tcp_write err=%d\r\n", (int)wr); break; }

                g_bench_bytes += to_send;

        err_t out = tcp_output(client_pcb);
        if (out != ERR_OK)
        {
            DebugUART_Print("[BENCH TX] tcp_output err=%d\r\n", (int)out);
            break;
        }

        if ((g_bench_bytes - g_bench_last_print) >= BENCH_PRINT_STEP)
                {
                    g_bench_last_print = g_bench_bytes;
                    Bench_PrintProgress();
                }
        }

    /* один tcp_output после всего цикла */
        if (client_pcb != NULL)
        {
            err_t out = tcp_output(client_pcb);
            if (out != ERR_OK)
            {
                DebugUART_Print("[BENCH TX] tcp_output err=%d\r\n", (int)out);
            }
        }
}

static void bench_tx_start_cb(void *arg)
{
    LWIP_UNUSED_ARG(arg);
    DebugUART_Print("[BENCH TX] pump started from tcpip_thread\r\n");
    Bench_TxPump();
}

#endif /* ETH_BENCHMARK_MODE == 2 */

/* ===== TX ring (используется только в режиме 0) ===== */

static uint32_t RawTcp_TxUsed(void)
{
    if (g_tx_head >= g_tx_tail) { return g_tx_head - g_tx_tail; }
    return RAW_TCP_TX_RING_SIZE - g_tx_tail + g_tx_head;
}

static uint32_t RawTcp_TxFree(void)
{
    return RAW_TCP_TX_RING_SIZE - RawTcp_TxUsed() - 1U;
}

static void RawTcp_TxReset(void)
{
    g_tx_head = 0;
    g_tx_tail = 0;
    g_tx_flush_scheduled = 0;
}

static int RawTcp_TxPush(const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U)) { return -1; }

    osKernelLock();

    if (RawTcp_TxFree() < len)
    {
        osKernelUnlock();
        g_tx_drop_count++;
        if ((g_tx_drop_count % 100U) == 0U)
        {
            DebugUART_Print("[TCP] TX ring drops=%lu\r\n",
                            (unsigned long)g_tx_drop_count);
        }
        return -2;
    }

    for (size_t i = 0; i < len; i++)
    {
        g_tx_ring[g_tx_head] = data[i];
        g_tx_head = (g_tx_head + 1U) % RAW_TCP_TX_RING_SIZE;
    }

    osKernelUnlock();
    return 0;
}

static uint32_t RawTcp_TxContiguousLen(void)
{
    if (g_tx_head == g_tx_tail)  { return 0; }
    if (g_tx_head > g_tx_tail)   { return g_tx_head - g_tx_tail; }
    return RAW_TCP_TX_RING_SIZE - g_tx_tail;
}

static void RawTcp_TxConsume(uint32_t len)
{
    g_tx_tail = (g_tx_tail + len) % RAW_TCP_TX_RING_SIZE;
}

static void RawTcp_TryFlush(void)
{
    if (client_pcb == NULL) { RawTcp_TxReset(); return; }

    for (;;)
    {
        uint32_t available = RawTcp_TxContiguousLen();
        if (available == 0U) { break; }

        u16_t sndbuf = tcp_sndbuf(client_pcb);
        if (sndbuf == 0U)   { break; }

        uint32_t to_send = available;
        if (to_send > sndbuf) { to_send = sndbuf; }
        if (to_send > 1460U)  { to_send = 1460U;  }
        if (to_send == 0U)    { break; }

        err_t wr = tcp_write(client_pcb,
                             &g_tx_ring[g_tx_tail],
                             (u16_t)to_send,
                             TCP_WRITE_FLAG_COPY);

        if (wr == ERR_MEM)
        {
            g_tcp_err_mem_count++;
            if ((g_tcp_err_mem_count % 100U) == 0U)
            {
                DebugUART_Print("[TCP] tcp_write ERR_MEM count=%lu sndbuf=%u\r\n",
                                (unsigned long)g_tcp_err_mem_count,
                                (unsigned)sndbuf);
            }
            break;
        }

        if (wr != ERR_OK)
        {
            DebugUART_Print("[TCP] tcp_write err=%d\r\n", (int)wr);
            break;
        }

        RawTcp_TxConsume(to_send);

        err_t out = tcp_output(client_pcb);
        if (out != ERR_OK)
        {
            DebugUART_Print("[TCP] tcp_output err=%d\r\n", (int)out);
            break;
        }
    }
}

static void raw_tcp_flush_cb(void *arg)
{
    LWIP_UNUSED_ARG(arg);
    g_tx_flush_scheduled = 0;
    RawTcp_TryFlush();
}

static void RawTcp_ScheduleFlush(void)
{
    if (client_pcb == NULL)      { return; }
    if (g_tx_flush_scheduled)    { return; }
    g_tx_flush_scheduled = 1;

    err_t cb_err = tcpip_callback(raw_tcp_flush_cb, NULL);
    if (cb_err != ERR_OK)
    {
        g_tx_flush_scheduled = 0;
        DebugUART_Print("[TCP] tcpip_callback(flush) err=%d\r\n", (int)cb_err);
    }
}

/* ===== CALLBACKS ===== */

static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(tpcb);
    LWIP_UNUSED_ARG(len);

#if (ETH_BENCHMARK_MODE == 2)
    Bench_TxPump();   /* отправили — пробуем залить ещё */
#else
    RawTcp_TryFlush();
#endif

    return ERR_OK;
}

static void tcp_server_error(void *arg, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    DebugUART_Print("[TCP] ERROR cb err=%d\r\n", (int)err);
    client_pcb = NULL;
    RawTcp_TxReset();
}

static err_t tcp_server_recv(void *arg,
                             struct tcp_pcb *tpcb,
                             struct pbuf *p,
                             err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if (p == NULL)
    {
        DebugUART_Print("[TCP] Client disconnected\r\n");

#if (ETH_BENCHMARK_MODE == 1)
        /* финальный отчёт при дисконнекте */
        if (g_bench_started)
        {
            Bench_PrintProgress();
            DebugUART_Print("[BENCH RX] === DONE (disconnect) ===\r\n");
        }
#endif

        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_err(tpcb, NULL);

        err_t close_err = tcp_close(tpcb);
        if (close_err != ERR_OK)
        {
            tcp_abort(tpcb);
        }

        client_pcb = NULL;
        RawTcp_TxReset();
        return ERR_OK;
    }

    if (err != ERR_OK)
    {
        DebugUART_Print("[TCP] RECV err=%d\r\n", (int)err);
        pbuf_free(p);
        return err;
    }

    tcp_recved(tpcb, p->tot_len);
    tcp_output(tpcb);   /* немедленно отправляем ACK, не ждём таймер */

    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        const uint16_t len = q->len;

#if (ETH_BENCHMARK_MODE == 1)
        /*
         * Тест 1: ПК -> плата.
         * Просто считаем байты, ничего не отвечаем.
         */
        if (!g_bench_started && len > 0)
        {
            g_bench_started       = 1;
            g_bench_start_tick    = osKernelGetTickCount();
            g_bench_start_tick_abs = g_bench_start_tick;
            DebugUART_Print("[BENCH RX] === START === tick=%lu ms\r\n",
                            (unsigned long)g_bench_start_tick);
        }

        if (!g_bench_done)
        {
            g_bench_bytes += len;

            if ((g_bench_bytes - g_bench_last_print) >= BENCH_PRINT_STEP)
            {
                g_bench_last_print = g_bench_bytes;
                Bench_PrintProgress();
            }

            if (g_bench_bytes >= BENCH_TARGET_BYTES)
            {
                g_bench_done = 1;

                uint32_t end_tick   = osKernelGetTickCount();
                uint32_t elapsed_ms = end_tick - g_bench_start_tick;
                uint32_t speed_kbps = 0;

                if (elapsed_ms > 0)
                {
                    speed_kbps = (uint32_t)((uint64_t)g_bench_bytes * 8ULL / elapsed_ms);
                }

                DebugUART_Print("[BENCH RX] === DONE ===\r\n");
                DebugUART_Print("[BENCH RX] start tick : %lu ms\r\n",
                                (unsigned long)g_bench_start_tick_abs);
                DebugUART_Print("[BENCH RX] end tick   : %lu ms\r\n",
                                (unsigned long)end_tick);
                DebugUART_Print("[BENCH RX] duration   : %lu ms\r\n",
                                (unsigned long)elapsed_ms);
                DebugUART_Print("[BENCH RX] total bytes: %lu\r\n",
                                (unsigned long)g_bench_bytes);
                DebugUART_Print("[BENCH RX] avg speed  : %lu Kbit/s (%lu Mbit/s)\r\n",
                                (unsigned long)speed_kbps,
                                (unsigned long)(speed_kbps / 1000UL));
            }
        }

#elif (ETH_BENCHMARK_MODE == 2)
        /*
         * Тест 2: плата -> ПК.
         * Входящие данные игнорируем (ПК ничего не шлёт).
         */
        (void)len;

#else
        /*
         * Обычный режим — передаём в ClientHandler.
         */
        ClientHandler_InputBytes((const uint8_t *)q->payload, len);
#endif
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_accept(void *arg,
                               struct tcp_pcb *newpcb,
                               err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if ((err != ERR_OK) || (newpcb == NULL))
    {
        DebugUART_Print("[TCP] ACCEPT err=%d\r\n", (int)err);
        return ERR_VAL;
    }

    if (client_pcb != NULL)
    {
        DebugUART_Print("[TCP] Reject 2nd client\r\n");
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    client_pcb = newpcb;
    RawTcp_TxReset();

#if (ETH_BENCHMARK_MODE == 1)
    Bench_Reset();
    DebugUART_Print("[BENCH RX] client connected, waiting for data...\r\n");
#endif

#if (ETH_BENCHMARK_MODE == 2)
    Bench_Reset();
    Bench_TxInit();
    DebugUART_Print("[BENCH TX] client connected, starting TX pump...\r\n");
    g_bench_started    = 1;
    g_bench_start_tick = osKernelGetTickCount();
    tcpip_callback(bench_tx_start_cb, NULL);
#endif

    DebugUART_Print("[TCP] ACCEPT from %d.%d.%d.%d:%u\r\n",
                    ip4_addr1(&newpcb->remote_ip),
                    ip4_addr2(&newpcb->remote_ip),
                    ip4_addr3(&newpcb->remote_ip),
                    ip4_addr4(&newpcb->remote_ip),
                    (unsigned)newpcb->remote_port);

    tcp_nagle_disable(newpcb);
    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, tcp_server_recv);
    tcp_err(newpcb, tcp_server_error);
    tcp_sent(newpcb, tcp_server_sent);

    return ERR_OK;
}

/* ===== PUBLIC API ===== */

int RawTcpServer_HasClient(void)
{
    return (client_pcb != NULL) ? 1 : 0;
}

int RawTcpServer_Send(const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U)) { return -1; }
    if (client_pcb == NULL)            { return -2; }
    if (RawTcp_TxPush(data, len) != 0) { return -3; }
    RawTcp_TryFlush();
    return 0;
}

int RawTcpServer_SendAsync(const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U)) { return -1; }
    if (client_pcb == NULL)            { return -2; }
    if (RawTcp_TxPush(data, len) != 0) { return -3; }
    RawTcp_ScheduleFlush();
    return 0;
}

/* ===== INIT ===== */

void RawTcpServer_Init(void)
{
    if (server_pcb != NULL)
    {
        DebugUART_Print("[TCP] previous server pcb exists\r\n");
        return;
    }

#if (ETH_BENCHMARK_MODE == 1)
    DebugUART_Print("[BENCH] MODE 1: PC -> STM32 (RX speed test)\r\n");
#elif (ETH_BENCHMARK_MODE == 2)
    DebugUART_Print("[BENCH] MODE 2: STM32 -> PC (TX speed test)\r\n");
#else
    DebugUART_Print("[TCP] Normal SLCAN mode\r\n");
#endif

    server_pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (server_pcb == NULL)
    {
        DebugUART_Print("[TCP] tcp_new_ip_type failed\r\n");
        return;
    }

    err_t err = tcp_bind(server_pcb, IP_ANY_TYPE, TCP_SERVER_PORT);
    if (err != ERR_OK)
    {
        DebugUART_Print("[TCP] tcp_bind failed err=%d\r\n", (int)err);
        tcp_close(server_pcb);
        server_pcb = NULL;
        return;
    }

    err_t err2 = ERR_OK;
    server_pcb = tcp_listen_with_backlog_and_err(server_pcb, 1, &err2);
    if (server_pcb == NULL)
    {
        DebugUART_Print("[TCP] tcp_listen failed err=%d\r\n", (int)err2);
        return;
    }

    tcp_accept(server_pcb, tcp_server_accept);
    DebugUART_Print("[TCP] Listening on port %d\r\n", TCP_SERVER_PORT);
}

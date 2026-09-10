/*
 * raw_tcp_server.c
 *
 * TCP RAW server (multi-client, up to MAX_CLIENTS) + UDP benchmark mode
 *
 * Обычный режим (ETH_BENCHMARK_MODE == 0) принимает до MAX_CLIENTS
 * одновременных подключений вместо одного.
 * Каждому клиенту выделяется слот в g_clients[], индекс слота = client_id,
 * передаётся дальше в client_handler.c во всех вызовах. Бенчмарк-режимы
 * (1/2/3) остаются однoклиентскими, они диагностические
 * инструменты, не часть основной задачи с несколькими клиентами.
 */

#include "raw_tcp_server.h"
#include "client_handler.h"
#include "core_task.h"
#include "app_queues.h"   /* MAX_CLIENTS */

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/inet.h"
#include "lwip/tcpip.h"

#include "cmsis_os.h"
#include "debug_uart.h"

#include <string.h>
#include <stdint.h>

#define TCP_SERVER_PORT 2001

/*
 * ETH_BENCHMARK_MODE:
 *   0 = обычный рабочий режим (SLCAN), многоклиентский
 *   1 = Тест 1: ПК -> плата. Плата только считает входящие байты (TCP).
 *   2 = Тест 2: плата -> ПК. Плата сама генерирует и льёт данные клиенту (TCP).
 *   3 = Тест 3: ПК -> плата. UDP — максимальная скорость без TCP overhead.
 */
#define ETH_BENCHMARK_MODE 0

#define BENCH_TARGET_BYTES  (1024UL * 1024UL * 1024UL)   /* 1 ГБ вместо 100 МБ */
#define BENCH_PRINT_STEP    (10UL  * 1024UL * 1024UL)

/*
 * TX-кольцо на КАЖДОГО клиента, 2 КБ x MAX_CLIENTS(4) = 8 КБ суммарно
 * столько же, сколько раньше занимал один общий буфер на 8 КБ.
 * Ответы SLCAN очень маленькие, 2 КБ на клиента
 * запас на сотни ожидающих отправки сообщений даже под нагрузкой.
 */
#define RAW_TCP_TX_RING_SIZE_PER_CLIENT   2048U

#define CLIENT_IDLE_TIMEOUT_MS   (10UL * 1000UL)

typedef struct
{
    struct tcp_pcb *pcb;
    uint32_t last_activity_tick;

    uint8_t           tx_ring[RAW_TCP_TX_RING_SIZE_PER_CLIENT];
    volatile uint32_t tx_head;
    volatile uint32_t tx_tail;
    volatile uint8_t  tx_flush_scheduled;

    uint32_t tx_drop_count;
    uint32_t tcp_err_mem_count;
} tcp_client_t;

static struct tcp_pcb *server_pcb = NULL;
static tcp_client_t g_clients[MAX_CLIENTS];

/* ===== Benchmark state (TCP MODE 1/2) -- остаются однoклиентскими ===== */
#if (ETH_BENCHMARK_MODE == 1) || (ETH_BENCHMARK_MODE == 2)

static struct tcp_pcb *g_bench_client_pcb = NULL;

static uint32_t g_bench_bytes      = 0;
static uint32_t g_bench_last_print = 0;
static uint32_t g_bench_start_tick = 0;
static uint32_t g_bench_start_tick_abs = 0;
static uint8_t  g_bench_started    = 0;
static uint8_t  g_bench_done       = 0;

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

    if (g_bench_client_pcb != NULL)
    {
        rcv_wnd = g_bench_client_pcb->rcv_wnd;
        rcv_nxt = g_bench_client_pcb->rcv_nxt;
    }

    DebugUART_Print("[BENCH] %lu MB | %lu ms | %lu Kbit/s (%lu Mbit/s) | rcv_wnd=%u rcv_nxt=%lu\r\n",
                    (unsigned long)(g_bench_bytes / (1024UL * 1024UL)),
                    (unsigned long)elapsed_ms,
                    (unsigned long)speed_kbps,
                    (unsigned long)(speed_kbps / 1000UL),
                    (unsigned)rcv_wnd,
                    (unsigned long)rcv_nxt);

    ETH_DebugPrintCounters("BENCH");
}

static void Bench_Reset(void)
{
    g_bench_bytes          = 0;
    g_bench_last_print     = 0;
    g_bench_start_tick     = 0;
    g_bench_start_tick_abs = 0;
    g_bench_started        = 0;
    g_bench_done           = 0;
}

#endif /* ETH_BENCHMARK_MODE 1 or 2 */

/* ===== UDP Benchmark (MODE 3) ===== */
#if (ETH_BENCHMARK_MODE == 3)

#define UDP_SERVER_PORT     2002U

static struct udp_pcb *udp_bench_pcb = NULL;

static uint32_t g_udp_bytes         = 0;
static uint32_t g_udp_last_print    = 0;
static uint32_t g_udp_start_tick    = 0;
static uint8_t  g_udp_started       = 0;
static uint8_t  g_udp_done          = 0;
static uint32_t g_udp_pkts_received = 0;

static void UdpBench_PrintProgress(void)
{
    uint32_t elapsed_ms = osKernelGetTickCount() - g_udp_start_tick;
    uint32_t speed_kbps = 0;

    if (elapsed_ms > 0)
    {
        speed_kbps = (uint32_t)((uint64_t)g_udp_bytes * 8ULL / elapsed_ms);
    }

    DebugUART_Print("[UDP BENCH] %lu MB | %lu ms | %lu Kbit/s (%lu Mbit/s)\r\n",
                    (unsigned long)(g_udp_bytes / (1024UL * 1024UL)),
                    (unsigned long)elapsed_ms,
                    (unsigned long)speed_kbps,
                    (unsigned long)(speed_kbps / 1000UL));

    DebugUART_Print("[UDP BENCH] pkts received=%lu\r\n",
                    (unsigned long)g_udp_pkts_received);
}

static void udp_bench_recv(void *arg,
                           struct udp_pcb *pcb,
                           struct pbuf *p,
                           const ip_addr_t *addr,
                           u16_t port)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(pcb);
    LWIP_UNUSED_ARG(addr);
    LWIP_UNUSED_ARG(port);

    if (p == NULL) { return; }

    if (!g_udp_started)
    {
        g_udp_started        = 1;
        g_udp_done           = 0;
        g_udp_start_tick     = osKernelGetTickCount();
        g_udp_pkts_received  = 0;
        DebugUART_Print("[UDP BENCH] === START === tick=%lu ms\r\n",
                        (unsigned long)g_udp_start_tick);
    }

    if (!g_udp_done)
    {
        g_udp_pkts_received++;
        g_udp_bytes += p->tot_len;

        if ((g_udp_bytes - g_udp_last_print) >= BENCH_PRINT_STEP)
        {
            g_udp_last_print = g_udp_bytes;
            UdpBench_PrintProgress();
        }

        if (g_udp_bytes >= BENCH_TARGET_BYTES)
        {
            g_udp_done = 1;

            uint32_t end_tick   = osKernelGetTickCount();
            uint32_t elapsed_ms = end_tick - g_udp_start_tick;
            uint32_t speed_kbps = 0;

            if (elapsed_ms > 0)
            {
                speed_kbps = (uint32_t)((uint64_t)g_udp_bytes * 8ULL / elapsed_ms);
            }

            uint32_t expected_pkts = (BENCH_TARGET_BYTES + 1399U) / 1400U;
            uint32_t lost_pkts     = (expected_pkts > g_udp_pkts_received)
                                     ? (expected_pkts - g_udp_pkts_received) : 0U;
            float    lost_pct      = (expected_pkts > 0)
                                     ? ((float)lost_pkts / (float)expected_pkts * 100.0f)
                                     : 0.0f;

            DebugUART_Print("[UDP BENCH] === DONE ===\r\n");
            DebugUART_Print("[UDP BENCH] start tick    : %lu ms\r\n",
                            (unsigned long)g_udp_start_tick);
            DebugUART_Print("[UDP BENCH] end tick      : %lu ms\r\n",
                            (unsigned long)end_tick);
            DebugUART_Print("[UDP BENCH] duration      : %lu ms\r\n",
                            (unsigned long)elapsed_ms);
            DebugUART_Print("[UDP BENCH] total bytes   : %lu\r\n",
                            (unsigned long)g_udp_bytes);
            DebugUART_Print("[UDP BENCH] pkts received : %lu\r\n",
                            (unsigned long)g_udp_pkts_received);
            DebugUART_Print("[UDP BENCH] pkts expected : %lu\r\n",
                            (unsigned long)expected_pkts);
            DebugUART_Print("[UDP BENCH] pkts lost     : %lu (%.1f%%)\r\n",
                            (unsigned long)lost_pkts,
                            (double)lost_pct);
            DebugUART_Print("[UDP BENCH] avg speed     : %lu Kbit/s (%lu Mbit/s)\r\n",
                            (unsigned long)speed_kbps,
                            (unsigned long)(speed_kbps / 1000UL));
        }
    }

    pbuf_free(p);
}

#endif /* ETH_BENCHMARK_MODE == 3 */

/* ===== Тест 2: генератор данных плата -> ПК ===== */
#if (ETH_BENCHMARK_MODE == 2)

#define BENCH_TX_BUF_SIZE   1460U

static uint8_t g_bench_tx_buf[BENCH_TX_BUF_SIZE];
static uint8_t g_bench_tx_buf_ready = 0;

static void Bench_TxInit(void)
{
    for (uint32_t i = 0; i < BENCH_TX_BUF_SIZE; i++)
    {
        g_bench_tx_buf[i] = (uint8_t)(i & 0xFFU);
    }
    g_bench_tx_buf_ready = 1;
}

static void Bench_TxPump(void)
{
    if (g_bench_client_pcb == NULL) { return; }
    if (!g_bench_tx_buf_ready)      { return; }
    if (g_bench_done)               { return; }

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

        u16_t sndbuf = tcp_sndbuf(g_bench_client_pcb);
        if (sndbuf < BENCH_TX_BUF_SIZE) { break; }

        uint32_t remaining = BENCH_TARGET_BYTES - g_bench_bytes;
        uint16_t to_send   = (remaining >= BENCH_TX_BUF_SIZE)
                             ? BENCH_TX_BUF_SIZE
                             : (uint16_t)remaining;

        err_t wr = tcp_write(g_bench_client_pcb, g_bench_tx_buf, to_send, TCP_WRITE_FLAG_COPY);
        if (wr == ERR_MEM) { break; }
        if (wr != ERR_OK)
        {
            DebugUART_Print("[BENCH TX] tcp_write err=%d\r\n", (int)wr);
            break;
        }

        g_bench_bytes += to_send;

        if ((g_bench_bytes - g_bench_last_print) >= BENCH_PRINT_STEP)
        {
            g_bench_last_print = g_bench_bytes;
            Bench_PrintProgress();
        }
    }

    if (g_bench_client_pcb != NULL)
    {
        err_t out = tcp_output(g_bench_client_pcb);
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

#if (ETH_BENCHMARK_MODE == 0)

/* ===== TX-кольцо конкретного клиента (обычный многоклиентский режим) ===== */

static uint32_t RawTcp_TxUsed(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];
    if (c->tx_head >= c->tx_tail) { return c->tx_head - c->tx_tail; }
    return RAW_TCP_TX_RING_SIZE_PER_CLIENT - c->tx_tail + c->tx_head;
}

static uint32_t RawTcp_TxFree(uint8_t id)
{
    return RAW_TCP_TX_RING_SIZE_PER_CLIENT - RawTcp_TxUsed(id) - 1U;
}

static void RawTcp_TxReset(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];
    c->tx_head            = 0;
    c->tx_tail             = 0;
    c->tx_flush_scheduled = 0;
}

static int RawTcp_TxPush(uint8_t id, const uint8_t *data, size_t len)
{
    tcp_client_t *c = &g_clients[id];

    if ((data == NULL) || (len == 0U)) { return -1; }

    osKernelLock();

    if (RawTcp_TxFree(id) < len)
    {
        osKernelUnlock();
        c->tx_drop_count++;
        if ((c->tx_drop_count % 100U) == 0U)
        {
            DebugUART_Print("[TCP] id=%u TX ring drops=%lu\r\n",
                            (unsigned)id, (unsigned long)c->tx_drop_count);
        }
        return -2;
    }

    for (size_t i = 0; i < len; i++)
    {
        c->tx_ring[c->tx_head] = data[i];
        c->tx_head = (c->tx_head + 1U) % RAW_TCP_TX_RING_SIZE_PER_CLIENT;
    }

    osKernelUnlock();
    return 0;
}

static uint32_t RawTcp_TxContiguousLen(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];
    if (c->tx_head == c->tx_tail) { return 0; }
    if (c->tx_head > c->tx_tail)  { return c->tx_head - c->tx_tail; }
    return RAW_TCP_TX_RING_SIZE_PER_CLIENT - c->tx_tail;
}

static void RawTcp_TxConsume(uint8_t id, uint32_t len)
{
    tcp_client_t *c = &g_clients[id];
    c->tx_tail = (c->tx_tail + len) % RAW_TCP_TX_RING_SIZE_PER_CLIENT;
}

static void RawTcp_TryFlush(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];

    if (c->pcb == NULL) { RawTcp_TxReset(id); return; }

    for (;;)
    {
        uint32_t available = RawTcp_TxContiguousLen(id);
        if (available == 0U) { break; }

        u16_t sndbuf = tcp_sndbuf(c->pcb);
        if (sndbuf == 0U)   { break; }

        uint32_t to_send = available;
        if (to_send > sndbuf) { to_send = sndbuf; }
        if (to_send > 1460U)  { to_send = 1460U;  }
        if (to_send == 0U)    { break; }

        err_t wr = tcp_write(c->pcb,
                             &c->tx_ring[c->tx_tail],
                             (u16_t)to_send,
                             TCP_WRITE_FLAG_COPY);

        if (wr == ERR_MEM)
        {
            c->tcp_err_mem_count++;
            break;
        }

        if (wr != ERR_OK)
        {
            DebugUART_Print("[TCP] id=%u tcp_write err=%d\r\n", (unsigned)id, (int)wr);
            break;
        }

        RawTcp_TxConsume(id, to_send);

        err_t out = tcp_output(c->pcb);
        if (out != ERR_OK)
        {
            DebugUART_Print("[TCP] id=%u tcp_output err=%d\r\n", (unsigned)id, (int)out);
            break;
        }
    }
}

static void raw_tcp_flush_cb(void *arg)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;
    if (id >= MAX_CLIENTS) { return; }
    g_clients[id].tx_flush_scheduled = 0;
    RawTcp_TryFlush(id);
}

static void RawTcp_ScheduleFlush(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];

    if (c->pcb == NULL)             { return; }
    if (c->tx_flush_scheduled)      { return; }
    c->tx_flush_scheduled = 1;

    err_t cb_err = tcpip_callback(raw_tcp_flush_cb, (void*)(uintptr_t)id);
    if (cb_err != ERR_OK)
    {
        c->tx_flush_scheduled = 0;
        DebugUART_Print("[TCP] id=%u tcpip_callback(flush) err=%d\r\n",
                        (unsigned)id, (int)cb_err);
    }
}

/* ===== CALLBACKS (многоклиентский обычный режим) ===== */

static void RawTcp_HandleClientGone(uint8_t id)
{
    g_clients[id].pcb = NULL;
    RawTcp_TxReset(id);
    CoreTask_NotifyClientGone(id);      /* ДО ClientHandler_ClientDisconnected --
                                            синтетическая команда "C\r" должна уйти
                                            в очередь до того, как обнулятся буферы
                                            клиента */
    ClientHandler_ClientDisconnected(id);
}

static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;
    LWIP_UNUSED_ARG(tpcb);
    LWIP_UNUSED_ARG(len);

    if (id >= MAX_CLIENTS) { return ERR_OK; }

    g_clients[id].last_activity_tick = osKernelGetTickCount();
    RawTcp_TryFlush(id);

    return ERR_OK;
}

static void tcp_server_error(void *arg, err_t err)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;

    if (id >= MAX_CLIENTS) { return; }

    DebugUART_Print("[TCP] id=%u ERROR cb err=%d\r\n", (unsigned)id, (int)err);
    DebugUART_Print("[TCP] id=%u TX drop count=%lu ERR_MEM count=%lu\r\n",
                    (unsigned)id,
                    (unsigned long)g_clients[id].tx_drop_count,
                    (unsigned long)g_clients[id].tcp_err_mem_count);

    RawTcp_HandleClientGone(id);
}

static err_t tcp_server_recv(void *arg,
                             struct tcp_pcb *tpcb,
                             struct pbuf *p,
                             err_t err)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;

    if (id >= MAX_CLIENTS)
    {
        if (p != NULL) { pbuf_free(p); }
        return ERR_OK;
    }

    g_clients[id].last_activity_tick = osKernelGetTickCount();

    if (p == NULL)
    {
        DebugUART_Print("[TCP] id=%u Client disconnected\r\n", (unsigned)id);

        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_err(tpcb, NULL);

        err_t close_err = tcp_close(tpcb);
        if (close_err != ERR_OK)
        {
            tcp_abort(tpcb);
        }

        RawTcp_HandleClientGone(id);
        return ERR_OK;
    }

    if (err != ERR_OK)
    {
        DebugUART_Print("[TCP] id=%u RECV err=%d\r\n", (unsigned)id, (int)err);
        pbuf_free(p);
        return err;
    }

    /*
     * tcp_output() здесь убран сознательно -- см. подробный комментарий
     * в истории версий файла: форсированный ACK на каждый пакет
     * конкурировал с RX-обработкой в tcpip_thread и вызывал нехватку
     * RX-буферов на высокой скорости. lwIP сам отправит ACK по своему
     * delayed-ACK таймеру.
     */
    tcp_recved(tpcb, p->tot_len);

    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        ClientHandler_InputBytes(id, (const uint8_t *)q->payload, q->len);
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_accept(void *arg,
                               struct tcp_pcb *newpcb,
                               err_t err)
{
    LWIP_UNUSED_ARG(arg);
    uint8_t free_id = MAX_CLIENTS;

    if ((err != ERR_OK) || (newpcb == NULL))
    {
        DebugUART_Print("[TCP] ACCEPT err=%d\r\n", (int)err);
        return ERR_VAL;
    }

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        if (g_clients[id].pcb == NULL)
        {
            free_id = id;
            break;
        }
    }

    if (free_id == MAX_CLIENTS)
    {
        DebugUART_Print("[TCP] Reject client -- all %u slots full\r\n",
                        (unsigned)MAX_CLIENTS);
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    g_clients[free_id].pcb = newpcb;
    g_clients[free_id].last_activity_tick = osKernelGetTickCount();
    RawTcp_TxReset(free_id);
    ClientHandler_ClientConnected(free_id);
    CoreTask_ClientConnected(free_id);

    DebugUART_Print("[TCP] id=%u ACCEPT from %d.%d.%d.%d:%u\r\n",
                    (unsigned)free_id,
                    ip4_addr1(&newpcb->remote_ip),
                    ip4_addr2(&newpcb->remote_ip),
                    ip4_addr3(&newpcb->remote_ip),
                    ip4_addr4(&newpcb->remote_ip),
                    (unsigned)newpcb->remote_port);

    tcp_nagle_disable(newpcb);
    tcp_arg(newpcb, (void*)(uintptr_t)free_id);
    tcp_recv(newpcb, tcp_server_recv);
    tcp_err(newpcb, tcp_server_error);
    tcp_sent(newpcb, tcp_server_sent);

    return ERR_OK;
}

#else /* ETH_BENCHMARK_MODE != 0 -- бенчмарк-режимы, однoклиентские, как раньше */

static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(tpcb);
    LWIP_UNUSED_ARG(len);

#if (ETH_BENCHMARK_MODE == 2)
    Bench_TxPump();
#endif

    return ERR_OK;
}

static void tcp_server_error(void *arg, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    DebugUART_Print("[TCP] ERROR cb err=%d\r\n", (int)err);
    ETH_DebugPrintCounters("TCP-ERR");

#if (ETH_BENCHMARK_MODE == 1)
    if (g_bench_started)
    {
        Bench_PrintProgress();
    }
    else
    {
        DebugUART_Print("[BENCH RX] error before first byte received\r\n");
    }
#endif

    g_bench_client_pcb = NULL;
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

        g_bench_client_pcb = NULL;
        return ERR_OK;
    }

    if (err != ERR_OK)
    {
        DebugUART_Print("[TCP] RECV err=%d\r\n", (int)err);
        pbuf_free(p);
        return err;
    }

    tcp_recved(tpcb, p->tot_len);

    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        const uint16_t len = q->len;

#if (ETH_BENCHMARK_MODE == 1)
        if (!g_bench_started && len > 0)
        {
            g_bench_started        = 1;
            g_bench_start_tick     = osKernelGetTickCount();
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
                DebugUART_Print("[BENCH RX] total bytes: %lu\r\n",
                                (unsigned long)g_bench_bytes);
                DebugUART_Print("[BENCH RX] avg speed  : %lu Kbit/s (%lu Mbit/s)\r\n",
                                (unsigned long)speed_kbps,
                                (unsigned long)(speed_kbps / 1000UL));
            }
        }

#elif (ETH_BENCHMARK_MODE == 2)
        (void)len;
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

    if (g_bench_client_pcb != NULL)
    {
        DebugUART_Print("[TCP] Reject 2nd client (benchmark mode -- single client only)\r\n");
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    g_bench_client_pcb = newpcb;

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

#endif /* ETH_BENCHMARK_MODE */

/* ===== PUBLIC API ===== */

int RawTcpServer_HasClient(uint8_t client_id)
{
#if (ETH_BENCHMARK_MODE == 0)
    if (client_id >= MAX_CLIENTS) { return 0; }
    return (g_clients[client_id].pcb != NULL) ? 1 : 0;
#else
    LWIP_UNUSED_ARG(client_id);
    return (g_bench_client_pcb != NULL) ? 1 : 0;
#endif
}

int RawTcpServer_Send(uint8_t client_id, const uint8_t *data, size_t len)
{
#if (ETH_BENCHMARK_MODE == 0)
    if ((data == NULL) || (len == 0U))        { return -1; }
    if (client_id >= MAX_CLIENTS)              { return -2; }
    if (g_clients[client_id].pcb == NULL)      { return -2; }
    if (RawTcp_TxPush(client_id, data, len) != 0) { return -3; }
    RawTcp_TryFlush(client_id);
    return 0;
#else
    LWIP_UNUSED_ARG(client_id);
    LWIP_UNUSED_ARG(data);
    LWIP_UNUSED_ARG(len);
    return -1;   /* не используется в бенчмарк-режимах */
#endif
}

int RawTcpServer_SendAsync(uint8_t client_id, const uint8_t *data, size_t len)
{
#if (ETH_BENCHMARK_MODE == 0)
    if ((data == NULL) || (len == 0U))        { return -1; }
    if (client_id >= MAX_CLIENTS)              { return -2; }
    if (g_clients[client_id].pcb == NULL)      { return -2; }
    if (RawTcp_TxPush(client_id, data, len) != 0) { return -3; }
    RawTcp_ScheduleFlush(client_id);
    return 0;
#else
    LWIP_UNUSED_ARG(client_id);
    LWIP_UNUSED_ARG(data);
    LWIP_UNUSED_ARG(len);
    return -1;
#endif
}

/*
 * Проверка "завис ли клиент" -- вызывать периодически из tcpip_thread.
 * В многоклиентском режиме проходит по ВСЕМ занятым слотам.
 */
void RawTcpServer_CheckIdleTimeout(void)
{
#if (ETH_BENCHMARK_MODE == 0)
    uint32_t now = osKernelGetTickCount();

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        if (g_clients[id].pcb == NULL) { continue; }

        if ((now - g_clients[id].last_activity_tick) > CLIENT_IDLE_TIMEOUT_MS)
        {
            DebugUART_Print("[TCP] id=%u idle timeout -- forcing abort\r\n", (unsigned)id);
            tcp_abort(g_clients[id].pcb);
            RawTcp_HandleClientGone(id);
        }
    }
#else
    /* бенчмарк-режимы: не трогаем -- диагностические тесты и так
     * ограничены по времени вручную (запуск/останов скрипта) */
#endif
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
#elif (ETH_BENCHMARK_MODE == 3)
    DebugUART_Print("[UDP BENCH] MODE 3: PC -> STM32 (UDP RX speed test)\r\n");
#else
    DebugUART_Print("[TCP] Normal SLCAN mode -- multi-client, MAX_CLIENTS=%u\r\n",
                    (unsigned)MAX_CLIENTS);
#endif

#if (ETH_BENCHMARK_MODE != 3)
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
#if (ETH_BENCHMARK_MODE == 0)
    /* backlog = MAX_CLIENTS, чтобы lwIP не отбрасывал SYN, пока принимаем
     * подключения по одному в tcp_server_accept() */
    server_pcb = tcp_listen_with_backlog_and_err(server_pcb, MAX_CLIENTS, &err2);
#else
    server_pcb = tcp_listen_with_backlog_and_err(server_pcb, 1, &err2);
#endif
    if (server_pcb == NULL)
    {
        DebugUART_Print("[TCP] tcp_listen failed err=%d\r\n", (int)err2);
        return;
    }

    tcp_accept(server_pcb, tcp_server_accept);
    DebugUART_Print("[TCP] Listening on port %d\r\n", TCP_SERVER_PORT);
#endif

#if (ETH_BENCHMARK_MODE == 3)
    udp_bench_pcb = udp_new_ip_type(IPADDR_TYPE_V4);

    if (udp_bench_pcb == NULL)
    {
        DebugUART_Print("[UDP BENCH] udp_new failed\r\n");
        return;
    }

    err_t udp_err = udp_bind(udp_bench_pcb, IP_ANY_TYPE, UDP_SERVER_PORT);

    if (udp_err != ERR_OK)
    {
        DebugUART_Print("[UDP BENCH] udp_bind failed err=%d\r\n", (int)udp_err);
        udp_remove(udp_bench_pcb);
        udp_bench_pcb = NULL;
        return;
    }

    udp_recv(udp_bench_pcb, udp_bench_recv, NULL);

    DebugUART_Print("[UDP BENCH] Listening on UDP port %u\r\n",
                    (unsigned)UDP_SERVER_PORT);
#endif
}

/*
 * raw_tcp_server.c
 *
 * TCP RAW server (single client) + UDP benchmark mode
 */

#include "raw_tcp_server.h"
#include "client_handler.h"

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
 *   0 = обычный рабочий режим (SLCAN)
 *   1 = Тест 1: ПК -> плата. Плата только считает входящие байты (TCP).
 *   2 = Тест 2: плата -> ПК. Плата сама генерирует и льёт данные клиенту (TCP).
 *   3 = Тест 3: ПК -> плата. UDP — максимальная скорость без TCP overhead.
 */
#define ETH_BENCHMARK_MODE 1

//#define BENCH_TARGET_BYTES  (100UL * 1024UL * 1024UL)
#define BENCH_TARGET_BYTES  (1024UL * 1024UL * 1024UL)   /* 1 ГБ вместо 100 МБ — */
#define BENCH_PRINT_STEP    (10UL  * 1024UL * 1024UL)

/* ===== TX ring (используется только в режиме 0) ===== */

#define RAW_TCP_TX_RING_SIZE 8192

static struct tcp_pcb *server_pcb = NULL;
static struct tcp_pcb *client_pcb = NULL;

static uint8_t g_tx_ring[RAW_TCP_TX_RING_SIZE];
static volatile uint32_t g_tx_head = 0;
static volatile uint32_t g_tx_tail = 0;
static volatile uint8_t g_tx_flush_scheduled = 0;

static uint32_t g_tx_drop_count = 0;
static uint32_t g_tcp_err_mem_count = 0;

/* ===== Benchmark state (TCP MODE 1/2) ===== */
#if (ETH_BENCHMARK_MODE == 1) || (ETH_BENCHMARK_MODE == 2)

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

    if (client_pcb != NULL)
    {
        rcv_wnd = client_pcb->rcv_wnd;
        rcv_nxt = client_pcb->rcv_nxt;
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

            /* Считаем сколько пакетов должно было прийти при chunk=1400 */
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
    if (client_pcb == NULL)       { return; }
    if (!g_bench_tx_buf_ready)    { return; }
    if (g_bench_done)             { return; }

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
        if (sndbuf < BENCH_TX_BUF_SIZE) { break; }

        uint32_t remaining = BENCH_TARGET_BYTES - g_bench_bytes;
        uint16_t to_send   = (remaining >= BENCH_TX_BUF_SIZE)
                             ? BENCH_TX_BUF_SIZE
                             : (uint16_t)remaining;

        err_t wr = tcp_write(client_pcb, g_bench_tx_buf, to_send, TCP_WRITE_FLAG_COPY);
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

/* ===== TX ring ===== */

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
    g_tx_head            = 0;
    g_tx_tail            = 0;
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
    if (g_tx_head == g_tx_tail) { return 0; }
    if (g_tx_head > g_tx_tail)  { return g_tx_head - g_tx_tail; }
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
    if (client_pcb == NULL)   { return; }
    if (g_tx_flush_scheduled) { return; }
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
    Bench_TxPump();
#else
    RawTcp_TryFlush();
#endif

    return ERR_OK;
}

static void tcp_server_error(void *arg, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    DebugUART_Print("[TCP] ERROR cb err=%d\r\n", (int)err);
    DebugUART_Print("[TCP] TX drop count=%lu ERR_MEM count=%lu\r\n",
                    (unsigned long)g_tx_drop_count,
                    (unsigned long)g_tcp_err_mem_count);

    /* Полная статистика ETH прямо в момент обрыва — особенно важно
       для случаев, когда сбой происходит ДО первого порогового
       принта Bench_PrintProgress (то есть меньше 10 МБ передано) и
       иначе остаётся совершенно не видно, что творилось внутри. */
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

    /*
     * tcp_output() здесь убран сознательно.
     *
     * tcp_recved() только сообщает lwIP, что окно приёма увеличилось;
     * САМ факт отправки ACK-сегмента lwIP решает через собственный
     * механизм delayed ACK (обычно раз на каждые 2 полных сегмента,
     * либо по таймеру ~200 мс). Форсированный tcp_output() на КАЖДЫЙ
     * входящий пакет заставлял low_level_output() гонять TX-путь
     * (и раньше — блокироваться на DMA) при каждом recv-событии,
     * что на высокой скорости конкурировало с обработкой RX внутри
     * того же tcpip_thread и вызывало нехватку RX-буферов (RBU).
     * lwIP сам вызовет tcp_output() из delayed-ACK таймера, когда
     * ACK действительно нужно отправить.
     */
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
        (void)len;

#else
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
#elif (ETH_BENCHMARK_MODE == 3)
    DebugUART_Print("[UDP BENCH] MODE 3: PC -> STM32 (UDP RX speed test)\r\n");
#else
    DebugUART_Print("[TCP] Normal SLCAN mode\r\n");
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
    server_pcb = tcp_listen_with_backlog_and_err(server_pcb, 1, &err2);
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

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

#define ETH_BENCHMARK_MODE 0   //ставим 1 если TCP → STM32 → просто считаем байты
// Без SLCAN, Чтобы проверить чистую скорость Ethernet, без влияния логики.

/*
 * Внутренний буфер ответов TCP.
 * Сюда складываются ответы z\r / Z\r / echo,
 * если lwIP прямо сейчас не готов их отправить.
 */
#define RAW_TCP_TX_RING_SIZE 8192

static struct tcp_pcb *server_pcb = NULL;
static struct tcp_pcb *client_pcb = NULL;

static uint8_t g_tx_ring[RAW_TCP_TX_RING_SIZE];
static volatile uint32_t g_tx_head = 0;
static volatile uint32_t g_tx_tail = 0;
static volatile uint8_t g_tx_flush_scheduled = 0;

static uint32_t g_tx_drop_count = 0;
static uint32_t g_tcp_err_mem_count = 0;

#if ETH_BENCHMARK_MODE
static uint32_t g_eth_bench_bytes = 0;
static uint32_t g_eth_bench_last_print = 0;
#endif

static uint32_t RawTcp_TxUsed(void)
{
    if (g_tx_head >= g_tx_tail)
    {
        return g_tx_head - g_tx_tail;
    }

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
    if ((data == NULL) || (len == 0U))
    {
        return -1;
    }

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
    if (g_tx_head == g_tx_tail)
    {
        return 0;
    }

    if (g_tx_head > g_tx_tail)
    {
        return g_tx_head - g_tx_tail;
    }

    return RAW_TCP_TX_RING_SIZE - g_tx_tail;
}

static void RawTcp_TxConsume(uint32_t len)
{
    g_tx_tail = (g_tx_tail + len) % RAW_TCP_TX_RING_SIZE;
}

static void RawTcp_TryFlush(void)
{
    if (client_pcb == NULL)
    {
        RawTcp_TxReset();
        return;
    }

    for (;;)
    {
        uint32_t available = RawTcp_TxContiguousLen();

        if (available == 0U)
        {
            break;
        }

        u16_t sndbuf = tcp_sndbuf(client_pcb);

        if (sndbuf == 0U)
        {
            break;
        }

        uint32_t to_send = available;

        if (to_send > sndbuf)
        {
            to_send = sndbuf;
        }

        if (to_send > 1460U)
        {
            to_send = 1460U;
        }

        if (to_send == 0U)
        {
            break;
        }

        err_t wr = tcp_write(client_pcb,
                             &g_tx_ring[g_tx_tail],
                             (u16_t)to_send,
                             TCP_WRITE_FLAG_COPY);

        if (wr == ERR_MEM)
        {
            g_tcp_err_mem_count++;

            if ((g_tcp_err_mem_count % 100U) == 0U)
            {
                DebugUART_Print("[TCP] tcp_write ERR_MEM count=%lu used=%lu free=%lu sndbuf=%u\r\n",
                                (unsigned long)g_tcp_err_mem_count,
                                (unsigned long)RawTcp_TxUsed(),
                                (unsigned long)RawTcp_TxFree(),
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
    if (client_pcb == NULL)
    {
        return;
    }

    if (g_tx_flush_scheduled)
    {
        return;
    }

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

    RawTcp_TryFlush();

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
        DebugUART_Print("[TCP] Client disconnected (p==NULL)\r\n");

#if ETH_BENCHMARK_MODE
        DebugUART_Print("[ETH BENCH] final received %lu bytes\r\n",
                        (unsigned long)g_eth_bench_bytes);
#endif

        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_err(tpcb, NULL);

        err_t close_err = tcp_close(tpcb);

        if (close_err != ERR_OK)
        {
            DebugUART_Print("[TCP] tcp_close err=%d -> abort\r\n", (int)close_err);
            tcp_abort(tpcb);
        }

        client_pcb = NULL;
        RawTcp_TxReset();

        return ERR_OK;
    }

    if (err != ERR_OK)
    {
        DebugUART_Print("[TCP] RECV err=%d -> drop\r\n", (int)err);
        pbuf_free(p);
        return err;
    }

    tcp_recved(tpcb, p->tot_len);

    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        const uint8_t *data = (const uint8_t *)q->payload;
        const uint16_t len = q->len;

#if ETH_BENCHMARK_MODE
        (void)data;

        g_eth_bench_bytes += len;

        if ((g_eth_bench_bytes - g_eth_bench_last_print) >= (10U * 1024U * 1024U))
        {
            g_eth_bench_last_print = g_eth_bench_bytes;

            DebugUART_Print("[ETH BENCH] received %lu bytes\r\n",
                            (unsigned long)g_eth_bench_bytes);
        }
#else
        ClientHandler_InputBytes(data, len);
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
        DebugUART_Print("[TCP] ACCEPT err=%d newpcb=%p\r\n",
                        (int)err,
                        (void*)newpcb);
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

#if ETH_BENCHMARK_MODE
    g_eth_bench_bytes = 0;
    g_eth_bench_last_print = 0;
    DebugUART_Print("[ETH BENCH] started, counting RX bytes only\r\n");
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

/*
 * ВНИМАНИЕ:
 * RawTcpServer_Send теперь тоже кладет данные в TX ring,
 * а не пишет напрямую в tcp_write.
 */
int RawTcpServer_Send(const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U))
    {
        return -1;
    }

    if (client_pcb == NULL)
    {
        return -2;
    }

    if (RawTcp_TxPush(data, len) != 0)
    {
        return -3;
    }

    RawTcp_TryFlush();

    return 0;
}

/*
 * Можно вызывать из обычных FreeRTOS task.
 * Данные копируются в ring buffer и потом досылаются из tcpip_thread.
 */
int RawTcpServer_SendAsync(const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U))
    {
        return -1;
    }

    if (client_pcb == NULL)
    {
        return -2;
    }

    if (RawTcp_TxPush(data, len) != 0)
    {
        return -3;
    }

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

#if ETH_BENCHMARK_MODE
    DebugUART_Print("[ETH BENCH] MODE ENABLED: TCP RX bytes counter only\r\n");
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

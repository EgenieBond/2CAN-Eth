/*
 * raw_tcp_server.c
 *
 * TCP RAW server -- ДВА независимых порта (по одному на физический
 * CAN-канал), общий "плоский" пул клиентов на оба порта сразу.
 *
 * Порт 2001 -> канал 0 (FDCAN1)
 * Порт 2002 -> канал 1 (FDCAN2)
 *
 * Один и тот же клиент (программа на ПК) может открыть ДВА отдельных
 * TCP-соединения -- к порту 2001 и к порту 2002 -- и работать с обеими
 * шинами одновременно и независимо; для платы это просто два разных
 * слота в общем пуле, каждый со своим channel_id.
 */

#include "raw_tcp_server.h"
#include "client_handler.h"
#include "core_task.h"
#include "app_queues.h"   /* MAX_CLIENTS, NUM_CHANNELS */

#include "lwip/tcp.h"
#include "lwip/inet.h"
#include "lwip/tcpip.h"

#include "cmsis_os.h"
#include "debug_uart.h"

#include <string.h>
#include <stdint.h>

/* Порт для каждого канала: индекс = channel_id */
static const uint16_t TCP_SERVER_PORT[NUM_CHANNELS] = { 2001U, 2002U };

#define RAW_TCP_TX_RING_SIZE_PER_CLIENT   2048U
#define CLIENT_IDLE_TIMEOUT_MS             (10UL * 1000UL)

typedef struct
{
    struct tcp_pcb *pcb;
    uint8_t          channel_id;   /* канал, к которому привязан этот клиент -- задаётся при accept, не меняется */
    uint32_t         last_activity_tick;

    uint8_t           tx_ring[RAW_TCP_TX_RING_SIZE_PER_CLIENT];
    volatile uint32_t tx_head;
    volatile uint32_t tx_tail;
    volatile uint8_t  tx_flush_scheduled;

    uint32_t tx_drop_count;
    uint32_t tcp_err_mem_count;
} tcp_client_t;

static struct tcp_pcb *g_server_pcb[NUM_CHANNELS] = { NULL };
static tcp_client_t g_clients[MAX_CLIENTS];

/* ===== TX-кольцо конкретного клиента ===== */

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
    c->tx_tail            = 0;
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

        err_t wr = tcp_write(c->pcb, &c->tx_ring[c->tx_tail], (u16_t)to_send, TCP_WRITE_FLAG_COPY);

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

    if (c->pcb == NULL)        { return; }
    if (c->tx_flush_scheduled) { return; }
    c->tx_flush_scheduled = 1;

    err_t cb_err = tcpip_callback(raw_tcp_flush_cb, (void*)(uintptr_t)id);
    if (cb_err != ERR_OK)
    {
        c->tx_flush_scheduled = 0;
        DebugUART_Print("[TCP] id=%u tcpip_callback(flush) err=%d\r\n", (unsigned)id, (int)cb_err);
    }
}

/* ===== CALLBACKS ===== */

static void RawTcp_HandleClientGone(uint8_t id)
{
    g_clients[id].pcb = NULL;
    RawTcp_TxReset(id);

    /* ДО ClientHandler_ClientDisconnected -- синтетическая команда
     * "C\r" должна уйти в очередь до того, как обнулятся буферы. */
    CoreTask_NotifyClientGone(id);
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

    DebugUART_Print("[TCP] id=%u ch%u ERROR cb err=%d\r\n",
                    (unsigned)id, (unsigned)g_clients[id].channel_id, (int)err);
    DebugUART_Print("[TCP] id=%u TX drop count=%lu ERR_MEM count=%lu\r\n",
                    (unsigned)id,
                    (unsigned long)g_clients[id].tx_drop_count,
                    (unsigned long)g_clients[id].tcp_err_mem_count);

    RawTcp_HandleClientGone(id);
}

static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
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
        DebugUART_Print("[TCP] id=%u ch%u Client disconnected\r\n",
                        (unsigned)id, (unsigned)g_clients[id].channel_id);

        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_err(tpcb, NULL);

        err_t close_err = tcp_close(tpcb);
        if (close_err != ERR_OK) { tcp_abort(tpcb); }

        RawTcp_HandleClientGone(id);
        return ERR_OK;
    }

    if (err != ERR_OK)
    {
        DebugUART_Print("[TCP] id=%u RECV err=%d\r\n", (unsigned)id, (int)err);
        pbuf_free(p);
        return err;
    }

    tcp_recved(tpcb, p->tot_len);

    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        ClientHandler_InputBytes(id, (const uint8_t *)q->payload, q->len);
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    /* arg здесь -- channel_id этого СЕРВЕРНОГО pcb, установлен заранее
     * в RawTcpServer_Init() через tcp_arg(server_pcb[ch], ...). Это
     * НЕ то же самое, что client_id -- ниже мы найдём свободный слот
     * в общем пуле и назначим client_id отдельно. */
    uint8_t channel_id = (uint8_t)(uintptr_t)arg;
    uint8_t free_id = MAX_CLIENTS;

    if ((err != ERR_OK) || (newpcb == NULL))
    {
        DebugUART_Print("[TCP] ch%u ACCEPT err=%d\r\n", (unsigned)channel_id, (int)err);
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
        DebugUART_Print("[TCP] ch%u Reject client -- all %u slots full (во всей системе)\r\n",
                        (unsigned)channel_id, (unsigned)MAX_CLIENTS);
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    g_clients[free_id].pcb = newpcb;
    g_clients[free_id].channel_id = channel_id;
    g_clients[free_id].last_activity_tick = osKernelGetTickCount();
    RawTcp_TxReset(free_id);

    ClientHandler_ClientConnected(free_id, channel_id);
    CoreTask_ClientConnected(free_id, channel_id);

    DebugUART_Print("[TCP] id=%u ch%u ACCEPT from %d.%d.%d.%d:%u (port %u)\r\n",
                    (unsigned)free_id, (unsigned)channel_id,
                    ip4_addr1(&newpcb->remote_ip),
                    ip4_addr2(&newpcb->remote_ip),
                    ip4_addr3(&newpcb->remote_ip),
                    ip4_addr4(&newpcb->remote_ip),
                    (unsigned)newpcb->remote_port,
                    (unsigned)TCP_SERVER_PORT[channel_id]);

    tcp_nagle_disable(newpcb);
    tcp_arg(newpcb, (void*)(uintptr_t)free_id);
    tcp_recv(newpcb, tcp_server_recv);
    tcp_err(newpcb, tcp_server_error);
    tcp_sent(newpcb, tcp_server_sent);

    return ERR_OK;
}

/* ===== PUBLIC API ===== */

int RawTcpServer_HasClient(uint8_t client_id)
{
    if (client_id >= MAX_CLIENTS) { return 0; }
    return (g_clients[client_id].pcb != NULL) ? 1 : 0;
}

int RawTcpServer_Send(uint8_t client_id, const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U))            { return -1; }
    if (client_id >= MAX_CLIENTS)                  { return -2; }
    if (g_clients[client_id].pcb == NULL)          { return -2; }
    if (RawTcp_TxPush(client_id, data, len) != 0)  { return -3; }
    RawTcp_TryFlush(client_id);
    return 0;
}

int RawTcpServer_SendAsync(uint8_t client_id, const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U))            { return -1; }
    if (client_id >= MAX_CLIENTS)                  { return -2; }
    if (g_clients[client_id].pcb == NULL)          { return -2; }
    if (RawTcp_TxPush(client_id, data, len) != 0)  { return -3; }
    RawTcp_ScheduleFlush(client_id);
    return 0;
}

void RawTcpServer_CheckIdleTimeout(void)
{
    uint32_t now = osKernelGetTickCount();

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        if (g_clients[id].pcb == NULL) { continue; }

        if ((now - g_clients[id].last_activity_tick) > CLIENT_IDLE_TIMEOUT_MS)
        {
            DebugUART_Print("[TCP] id=%u ch%u idle timeout -- forcing abort\r\n",
                            (unsigned)id, (unsigned)g_clients[id].channel_id);
            tcp_abort(g_clients[id].pcb);
            RawTcp_HandleClientGone(id);
        }
    }
}

/* ===== INIT ===== */

void RawTcpServer_Init(void)
{
    DebugUART_Print("[TCP] Normal SLCAN mode -- multi-channel, NUM_CHANNELS=%u, MAX_CLIENTS=%u\r\n",
                    (unsigned)NUM_CHANNELS, (unsigned)MAX_CLIENTS);

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++)
    {
        if (g_server_pcb[ch] != NULL)
        {
            DebugUART_Print("[TCP] ch%u previous server pcb exists\r\n", (unsigned)ch);
            continue;
        }

        struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (pcb == NULL)
        {
            DebugUART_Print("[TCP] ch%u tcp_new_ip_type failed\r\n", (unsigned)ch);
            continue;
        }

        err_t err = tcp_bind(pcb, IP_ANY_TYPE, TCP_SERVER_PORT[ch]);
        if (err != ERR_OK)
        {
            DebugUART_Print("[TCP] ch%u tcp_bind failed err=%d\r\n", (unsigned)ch, (int)err);
            tcp_close(pcb);
            continue;
        }

        err_t err2 = ERR_OK;
        /* backlog = MAX_CLIENTS -- с запасом, раз пул общий на оба канала */
        pcb = tcp_listen_with_backlog_and_err(pcb, MAX_CLIENTS, &err2);
        if (pcb == NULL)
        {
            DebugUART_Print("[TCP] ch%u tcp_listen failed err=%d\r\n", (unsigned)ch, (int)err2);
            continue;
        }

        g_server_pcb[ch] = pcb;

        /* channel_id этого серверного pcb -- чтобы tcp_server_accept()
         * знал, на какой порт/канал пришло очередное подключение. */
        tcp_arg(pcb, (void*)(uintptr_t)ch);
        tcp_accept(pcb, tcp_server_accept);

        DebugUART_Print("[TCP] ch%u Listening on port %u\r\n", (unsigned)ch, (unsigned)TCP_SERVER_PORT[ch]);
    }
}

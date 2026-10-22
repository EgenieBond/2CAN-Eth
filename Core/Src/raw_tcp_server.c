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
 *
 * Обновлено (непрерывный поток):
 *  - страховка от потерянного пробуждения RX back-pressure
 *    (RawTcpServer_KickPendingRx), приём отложенных байтов считается
 *    активностью для idle timeout;
 *  - страховка от потерянного пробуждения TX: если TX-кольцо клиента
 *    полно, RawTcpServer_SendAsync всё равно планирует flush (раньше после
 *    ERR_MEM в tcp_write кольцо могло остаться полным навсегда);
 *  - диагностика: сколько pbuf'ов максимум удерживалось в rx_pending;
 *  - idle timeout больше не вызывает HandleClientGone дважды;
 *  - счётчики TX сбрасываются при новом подключении.
 */

#include "raw_tcp_server.h"
#include "client_handler.h"
#include "core_task.h"
#include "app_queues.h"   /* MAX_CLIENTS, NUM_CHANNELS */

#include "lwip/tcp.h"
#include "lwip/inet.h"
#include "lwip/tcpip.h"
#include "lwip/pbuf.h"

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

    /* RX backpressure: остаток входящих данных, которые пока некуда
     * положить. tcp_recved() для них не зовём -- TCP-окно клиента сужается
     * ровно на этот объём. Дообрабатывается в RawTcp_FlushPendingRx. */
    struct pbuf *rx_pending;

    /* Диагностика: максимум удерживаемых в rx_pending pbuf'ов и байт.
     * Если max_clen подходит к числу RX-буферов Ethernet-драйвера --
     * удержание pbuf'ов душит приём всего Ethernet (не только этого
     * клиента). */
    uint16_t rx_pending_max_clen;
    uint32_t rx_pending_max_bytes;
} tcp_client_t;

static struct tcp_pcb *g_server_pcb[NUM_CHANNELS] = { NULL };
static tcp_client_t g_clients[MAX_CLIENTS];

/* ===== Диагностика ===== */

static void RawTcp_TrackPending(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];
    u16_t clen;

    if (c->rx_pending == NULL) { return; }

    clen = pbuf_clen(c->rx_pending);
    if (clen > c->rx_pending_max_clen)                  { c->rx_pending_max_clen  = clen; }
    if (c->rx_pending->tot_len > c->rx_pending_max_bytes) { c->rx_pending_max_bytes = c->rx_pending->tot_len; }
}

static void RawTcp_PrintClientStats(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];

    DebugUART_Print("[TCP] id=%u TX drop count=%lu ERR_MEM count=%lu "
                    "rx_pending max: pbufs=%u bytes=%lu\r\n",
                    (unsigned)id,
                    (unsigned long)c->tx_drop_count,
                    (unsigned long)c->tcp_err_mem_count,
                    (unsigned)c->rx_pending_max_clen,
                    (unsigned long)c->rx_pending_max_bytes);
}

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
        /* Это не потеря данных: батч остаётся у ClientHandler и будет
         * предложен снова. Печатаем редко, чтобы не душить систему UART'ом. */
        if ((c->tx_drop_count == 1U) || ((c->tx_drop_count % 1000U) == 0U))
        {
            DebugUART_Print("[TCP] id=%u TX ring full (push refused) count=%lu\r\n",
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
            /* Нехватка памяти lwIP. Повтор произойдёт либо из sent-колбэка
             * (если есть неподтверждённые данные), либо из следующего
             * RawTcpServer_SendAsync -- он теперь планирует flush даже при
             * полном кольце, см. ниже. */
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

/* ===== RX BACKPRESSURE ("страховка": CAN не успевает -- тормозим TCP) =====
 *
 * Очередной входящий pbuf принимается ЧАСТЯМИ: забираем столько байт,
 * сколько реально сейчас влезает в rx_cmd_ring клиента (client_handler.c),
 * а остаток откладываем в rx_pending и подтверждаем (tcp_recved) только
 * тот кусок, который действительно приняли. Так TCP-окно клиента
 * сужается пропорционально занятости буферов, а не падает в ноль. */

/* Порог свободных слотов в core_to_can_queue канала, ниже которого
 * считаем канал перегруженным (очередь слотов -- страховка бинарная). */
#define CAN_QUEUE_BACKPRESSURE_LOW_WATER   32U

/* То же для eth_to_core_queue -- ОДНОЙ общей очереди на все каналы. */
#define ETH_TO_CORE_QUEUE_BACKPRESSURE_LOW_WATER   64U

/* Неприкасаемый запас в rx_cmd_ring клиента (байт). */
#define CLIENT_RX_RING_BACKPRESSURE_LOW_WATER_BYTES   2048U

/* Один общий флаг на все каналы/клиентов, чтобы не заваливать
 * tcpip_callback() повторными запросами, пока предыдущий не отработал. */
static volatile uint8_t g_backpressure_poll_scheduled = 0;
static uint32_t g_backpressure_poll_cb_fail = 0;

static int RawTcp_ChannelHasRoom(uint8_t channel_id)
{
    if ((eth_to_core_queue != NULL) &&
        (osMessageQueueGetSpace(eth_to_core_queue) < ETH_TO_CORE_QUEUE_BACKPRESSURE_LOW_WATER))
    {
        return 0;
    }

    if (channel_id >= NUM_CHANNELS)             { return 1; }
    if (core_to_can_queues[channel_id] == NULL) { return 1; }
    return (osMessageQueueGetSpace(core_to_can_queues[channel_id]) >= CAN_QUEUE_BACKPRESSURE_LOW_WATER) ? 1 : 0;
}

/* Сколько байт из входящей пачки длиной total_len можно реально принять
 * ПРЯМО СЕЙЧАС -- не "всё или ничего". */
static uint32_t RawTcp_ClientAcceptableBytes(uint8_t id, uint32_t total_len)
{
    uint32_t ring_free;
    uint32_t usable;

    if (!RawTcp_ChannelHasRoom(g_clients[id].channel_id)) { return 0U; }

    ring_free = ClientHandler_RxRingFreeBytes(id);
    if (ring_free <= CLIENT_RX_RING_BACKPRESSURE_LOW_WATER_BYTES) { return 0U; }

    usable = ring_free - CLIENT_RX_RING_BACKPRESSURE_LOW_WATER_BYTES;
    return (usable < total_len) ? usable : total_len;
}

/* Отдаёт в ClientHandler ровно accept_len байт из НАЧАЛА цепочки
 * пбуфов p и возвращает указатель на остаток цепочки. Сначала читаем
 * байты, потом обрезаем цепочку -- порядок важен. */
static struct pbuf *RawTcp_ConsumeFrontBytes(uint8_t id, struct pbuf *p, uint32_t accept_len)
{
    uint32_t remaining = accept_len;
    struct pbuf *q = p;

    while ((q != NULL) && (remaining > 0U))
    {
        uint32_t take = (remaining < q->len) ? remaining : q->len;
        if (take > 0U)
        {
            ClientHandler_InputBytes(id, (const uint8_t *)q->payload, take);
        }
        remaining -= take;
        if (take < q->len) { break; }   /* этот сегмент принят не целиком -- дальше не идём */
        q = q->next;
    }

    return pbuf_free_header(p, (u16_t)accept_len);
}

/* Дообрабатывает отложенный остаток клиента id: забирает из rx_pending
 * столько, сколько сейчас реально влезает, подтверждает клиенту
 * (tcp_recved) ровно этот кусок, остальное оставляет на следующий раз. */
static void RawTcp_FlushPendingRx(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];
    struct pbuf *p;
    uint32_t accept_len;

    if (c->rx_pending == NULL) { return; }

    if (c->pcb == NULL)
    {
        /* клиент уже отключился, пока ждали -- просто освобождаем */
        pbuf_free(c->rx_pending);
        c->rx_pending = NULL;
        return;
    }

    accept_len = RawTcp_ClientAcceptableBytes(id, c->rx_pending->tot_len);
    if (accept_len == 0U) { return; }

    /* Приём отложенных данных -- это реальная активность клиента: он шлёт,
     * мы принимаем, просто медленнее (back-pressure). Без этого при долгом
     * штатном торможении соединение убивалось бы по idle timeout. */
    c->last_activity_tick = osKernelGetTickCount();

    p = c->rx_pending;

    tcp_recved(c->pcb, (u16_t)accept_len);

    if (accept_len >= p->tot_len)
    {
        c->rx_pending = NULL;
        for (struct pbuf *q = p; q != NULL; q = q->next)
        {
            ClientHandler_InputBytes(id, (const uint8_t *)q->payload, q->len);
        }
        pbuf_free(p);
    }
    else
    {
        c->rx_pending = RawTcp_ConsumeFrontBytes(id, p, accept_len);
    }
}

static void raw_tcp_backpressure_poll_cb(void *arg)
{
    LWIP_UNUSED_ARG(arg);
    g_backpressure_poll_scheduled = 0;

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        RawTcp_FlushPendingRx(id);
    }
}

/*
 * Вызывается из CanTask каждый раз, когда из core_to_can_queue канала
 * channel_id реально забрали кадр. CanTask работает не в tcpip-потоке,
 * поэтому проверку и tcp_recved() откладываем через tcpip_callback().
 *
 * tcpip_callback() может не пройти (mailbox tcpip-потока полон под
 * нагрузкой) -- тогда уведомление теряется; от "последнего потерянного
 * пробуждения" страхует RawTcpServer_KickPendingRx().
 */
void RawTcpServer_NotifyChannelDrained(uint8_t channel_id)
{
    LWIP_UNUSED_ARG(channel_id);   /* колбэк всё равно проверяет все каналы разом -- дёшево */

    if (g_backpressure_poll_scheduled) { return; }
    g_backpressure_poll_scheduled = 1;

    if (tcpip_callback(raw_tcp_backpressure_poll_cb, NULL) != ERR_OK)
    {
        g_backpressure_poll_scheduled = 0;
        g_backpressure_poll_cb_fail++;
        if ((g_backpressure_poll_cb_fail == 1U) || ((g_backpressure_poll_cb_fail % 1000U) == 0U))
        {
            DebugUART_Print("[TCP] WARNING: tcpip_callback(backpressure poll) failed, count=%lu\r\n",
                            (unsigned long)g_backpressure_poll_cb_fail);
        }
    }
}

/*
 * Страховка от потерянного пробуждения: вызывается из ClientHandlerTask
 * каждую 1 мс. Если у какого-то клиента висит rx_pending -- запрашиваем
 * проверку, не дожидаясь CanTask. rx_pending читается здесь из чужого
 * потока без блокировки -- нужен только признак "не NULL", в худшем
 * случае будет один лишний или пропущенный пинок.
 */
void RawTcpServer_KickPendingRx(void)
{
    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        if (g_clients[id].rx_pending != NULL)
        {
            RawTcpServer_NotifyChannelDrained(g_clients[id].channel_id);
            return;   /* колбэк обходит всех клиентов сам */
        }
    }
}

/* ===== CALLBACKS ===== */

static void RawTcp_HandleClientGone(uint8_t id)
{
    g_clients[id].pcb = NULL;
    RawTcp_TxReset(id);

    if (g_clients[id].rx_pending != NULL)
    {
        pbuf_free(g_clients[id].rx_pending);
        g_clients[id].rx_pending = NULL;
    }

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
    RawTcp_PrintClientStats(id);
    DebugUART_Print("==================== CONNECTION CLOSED (error) ====================\r\n\r\n");

    RawTcp_HandleClientGone(id);
}

static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;
    uint32_t accept_len;

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
        RawTcp_PrintClientStats(id);
        DebugUART_Print("==================== CONNECTION CLOSED ====================\r\n\r\n");

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

    /* Если уже есть отложенный "хвост" -- обязаны сохранить порядок байт,
     * поэтому новый pbuf доклеиваем к нему; tcp_recved() для него тоже
     * пока не зовём. */
    if (g_clients[id].rx_pending != NULL)
    {
        pbuf_cat(g_clients[id].rx_pending, p);
        RawTcp_TrackPending(id);
        return ERR_OK;
    }

    accept_len = RawTcp_ClientAcceptableBytes(id, p->tot_len);

    if (accept_len == 0U)
    {
        g_clients[id].rx_pending = p;
        RawTcp_TrackPending(id);
        return ERR_OK;
    }

    tcp_recved(tpcb, (u16_t)accept_len);

    if (accept_len >= p->tot_len)
    {
        for (struct pbuf *q = p; q != NULL; q = q->next)
        {
            ClientHandler_InputBytes(id, (const uint8_t *)q->payload, q->len);
        }
        pbuf_free(p);
    }
    else
    {
        g_clients[id].rx_pending = RawTcp_ConsumeFrontBytes(id, p, accept_len);
        RawTcp_TrackPending(id);
    }

    return ERR_OK;
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    /* arg здесь -- channel_id этого СЕРВЕРНОГО pcb (см. RawTcpServer_Init).
     * client_id назначается отдельно -- первый свободный слот в общем пуле. */
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
    g_clients[free_id].tx_drop_count = 0;
    g_clients[free_id].tcp_err_mem_count = 0;
    g_clients[free_id].rx_pending_max_clen = 0;
    g_clients[free_id].rx_pending_max_bytes = 0;
    RawTcp_TxReset(free_id);

    ClientHandler_ClientConnected(free_id, channel_id);
    CoreTask_ClientConnected(free_id, channel_id);

    DebugUART_Print("\r\n==================== NEW CONNECTION ====================\r\n");
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

    if (RawTcp_TxPush(client_id, data, len) != 0)
    {
        /* Кольцо полно. Всё равно просим tcpip-поток попробовать его
         * протолкнуть: если прошлый tcp_write упал с ERR_MEM, а
         * неподтверждённых данных в полёте нет, sent-колбэк не придёт
         * никогда -- и без этого пинка кольцо осталось бы полным навсегда
         * (потерянное пробуждение на TX-стороне). ClientHandler повторяет
         * SendAsync каждую 1 мс, так что попытки идут регулярно. */
        RawTcp_ScheduleFlush(client_id);
        return -3;
    }

    RawTcp_ScheduleFlush(client_id);
    return 0;
}

void RawTcpServer_CheckIdleTimeout(void)
{
    uint32_t now = osKernelGetTickCount();

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        struct tcp_pcb *pcb = g_clients[id].pcb;

        if (pcb == NULL) { continue; }

        if ((now - g_clients[id].last_activity_tick) > CLIENT_IDLE_TIMEOUT_MS)
        {
            DebugUART_Print("[TCP] id=%u ch%u idle timeout -- forcing abort\r\n",
                            (unsigned)id, (unsigned)g_clients[id].channel_id);
            RawTcp_PrintClientStats(id);
            DebugUART_Print("==================== CONNECTION CLOSED (idle timeout) ====================\r\n\r\n");

            /* Отцепляем колбэки ДО tcp_abort: иначе tcp_abort вызовет
             * tcp_server_error, тот сделает HandleClientGone, и ниже мы
             * сделали бы его второй раз (двойной "C\r" и двойной лог). */
            tcp_arg(pcb, NULL);
            tcp_recv(pcb, NULL);
            tcp_sent(pcb, NULL);
            tcp_err(pcb, NULL);
            tcp_abort(pcb);

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

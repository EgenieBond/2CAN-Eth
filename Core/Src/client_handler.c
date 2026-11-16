/*
 * client_handler.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *  Каждый слот клиента помнит свой channel_id (задаётся один раз при
 *  подключении), и этот channel_id прописывается в каждую команду
 *  (eth_cmd_msg_t), уходящую в CoreTask.
 *
 *  core_to_eth_queue -- ОБЩАЯ на всю систему; ClientHandler_PollTx()
 *  раскладывает её по персональным TX-буферам клиентов (по client_id).
 *
 *  Обновлено (непрерывный поток, порядок ответов): пока у клиента есть
 *  pending_resp, ничего новее в его tx_batch не попадает -- см.
 *  ClientHandler_TryPlacePending().
 *
 *  Обновлено (RX flow control без удержания pbuf'ов):
 *  raw_tcp_server.c больше НЕ держит у себя входящие pbuf'ы (это были
 *  DMA-буферы Ethernet, их всего 13 на весь входящий трафик) -- данные
 *  сразу копируются сюда, в rx_cmd_ring, а pbuf тут же освобождается.
 *  Подтверждение приёма (tcp_recved) откладывается: байты "кредитуются"
 *  (recv_credit) только когда команда уходит из кольца дальше по
 *  конвейеру (или отбрасывается как некорректная). raw_tcp_server.c
 *  периодически забирает кредит и вызывает tcp_recved().
 *
 *  Почему кольцо не переполнится: неподтверждённых байт по определению
 *  не больше TCP_WND (больше ПК прислать не может). Кольцо хранит РОВНО
 *  байты TCP-потока, без разделителей: каждая команда в нём заканчивается
 *  на '\r' (в кольцо кладётся только полная строка), и граница команды
 *  находится по этому '\r'. Значит, кольцу достаточно TCP_WND байт --
 *  это проверяется при компиляции.
 */

#include "client_handler.h"
#include "cmsis_os.h"
#include "app_queues.h"
#include "debug_uart.h"
#include "event_log.h"
#include "fake_client_source.h"
#include "raw_tcp_server.h"
#include "lwip/opt.h"     /* TCP_WND -- для проверки размера кольца */

#include <string.h>
#include <stdint.h>

#define CLIENT_RX_BUFFER_SIZE        128
#define CLIENT_RX_CMD_RING_SIZE      (8U * 1024U)    /* >= TCP_WND + 1, см. _Static_assert ниже */
#define CLIENT_RX_CMD_DRAIN_LIMIT    256
#define CLIENT_TX_BATCH_SIZE         1024
#define CLIENT_TX_DRAIN_LIMIT        1024

#define CLIENT_USE_FAKE_SOURCE       0

/* Гарантия, что неподтверждённые (не прошедшие tcp_recved) байты всегда
 * помещаются в кольцо. Если увеличиваешь TCP_WND в lwipopts.h -- увеличь
 * и кольцо, иначе сборка упадёт здесь (а не плата начнёт терять данные). */
_Static_assert((CLIENT_RX_CMD_RING_SIZE - 1U) >= (uint32_t)TCP_WND,
               "CLIENT_RX_CMD_RING_SIZE must hold TCP_WND bytes (all unacknowledged TCP data)");

static osThreadId_t clientHandlerTaskHandle = NULL;

typedef struct
{
    uint8_t  in_use;
    uint8_t  channel_id;   /* канал, к которому привязан клиент -- задаётся при подключении */

    /* --- приём: TCP -> строки -> RX-кольцо -> eth_to_core_queue --- */
    char     rx_line_buf[CLIENT_RX_BUFFER_SIZE];
    size_t   rx_line_pos;

    uint8_t           rx_cmd_ring[CLIENT_RX_CMD_RING_SIZE];
    volatile uint32_t rx_cmd_head;
    volatile uint32_t rx_cmd_tail;

    /* Сколько байт TCP-потока уже ушло из нашего хранения (команда отдана
     * в eth_to_core или отброшена) и может быть подтверждено через
     * tcp_recved(). Пишется из двух потоков (tcpip -- при отбрасывании,
     * ClientHandler -- при извлечении), поэтому только под osKernelLock. */
    volatile uint32_t recv_credit;

    eth_cmd_msg_t pending_cmd;
    uint8_t       pending_cmd_valid;

    /* --- передача: core_to_eth_queue -> TX-буфер -> TCP --- */
    uint8_t  tx_batch[CLIENT_TX_BATCH_SIZE];
    size_t   tx_len;

    /* Один отложенный resp. ИНВАРИАНТ: пока pending_resp_valid, ничего
     * новее в tx_batch этого клиента не дописывается. */
    eth_resp_msg_t pending_resp;
    uint8_t        pending_resp_valid;

    /* --- диагностика --- */
    uint32_t dropped_rx_cmd_ring;      /* не должно случаться никогда (кольцо >= 2*TCP_WND) */
    uint32_t dropped_eth_to_core;      /* НЕ потери: сколько раз eth_to_core была полна и команда ждала в pending_cmd */
    uint32_t tcp_send_fail_count;
    uint32_t dropped_cmd_too_long;     /* ClientHandler_PushCmdToRxRing: len >= ETH_CMD_MAX_LEN */
    uint32_t dropped_line_too_long;    /* ClientHandler_InputBytes: строка длиннее rx_line_buf */
    uint32_t dropped_resp_overflow;    /* РЕАЛЬНЫЕ потери ответов: TX к ПК застрял, а pending_resp уже занят */
} client_slot_t;

static client_slot_t g_clients[MAX_CLIENTS];

/* ===== Кредит для tcp_recved ===== */

/* Вызывать БЕЗ взятого osKernelLock. */
static void ClientHandler_AddRecvCredit(uint8_t id, uint32_t bytes)
{
    if (bytes == 0U) { return; }
    osKernelLock();
    g_clients[id].recv_credit += bytes;
    osKernelUnlock();
}

/* Забирает накопленный кредит клиента (и обнуляет его). Вызывается из
 * tcpip-потока (raw_tcp_server.c) перед tcp_recved(). */
uint32_t ClientHandler_TakeRecvCredit(uint8_t client_id)
{
    uint32_t credit;

    if (client_id >= MAX_CLIENTS) { return 0U; }

    osKernelLock();
    credit = g_clients[client_id].recv_credit;
    g_clients[client_id].recv_credit = 0U;
    osKernelUnlock();

    return credit;
}

static int ClientHandler_AnyRecvCredit(void)
{
    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        /* чтение без блокировки -- нужен только признак "не ноль" */
        if (g_clients[id].in_use && (g_clients[id].recv_credit != 0U)) { return 1; }
    }
    return 0;
}

/* ===== RX-кольцо конкретного клиента ===== */

static uint32_t ClientHandler_RxRingUsed(uint8_t id)
{
    client_slot_t *c = &g_clients[id];
    if (c->rx_cmd_head >= c->rx_cmd_tail) { return c->rx_cmd_head - c->rx_cmd_tail; }
    return CLIENT_RX_CMD_RING_SIZE - c->rx_cmd_tail + c->rx_cmd_head;
}

static uint32_t ClientHandler_RxRingFree(uint8_t id)
{
    return CLIENT_RX_CMD_RING_SIZE - ClientHandler_RxRingUsed(id) - 1U;
}

static void ClientHandler_RxRingWriteByte(uint8_t id, uint8_t b)
{
    client_slot_t *c = &g_clients[id];
    c->rx_cmd_ring[c->rx_cmd_head] = b;
    c->rx_cmd_head = (c->rx_cmd_head + 1U) % CLIENT_RX_CMD_RING_SIZE;
}

static uint8_t ClientHandler_RxRingReadByte(uint8_t id)
{
    client_slot_t *c = &g_clients[id];
    uint8_t b = c->rx_cmd_ring[c->rx_cmd_tail];
    c->rx_cmd_tail = (c->rx_cmd_tail + 1U) % CLIENT_RX_CMD_RING_SIZE;
    return b;
}

/* Кладёт команду (строку, заканчивающуюся на \r) в кольцо. Вызывается из
 * tcpip-потока (через ClientHandler_InputBytes). Если команда отбрасывается,
 * её байты сразу кредитуются -- иначе они никогда не были бы подтверждены
 * и TCP-окно навсегда уменьшилось бы на их размер. */
static int ClientHandler_PushCmdToRxRing(uint8_t id, const char *cmd)
{
    client_slot_t *c = &g_clients[id];
    size_t len;

    if (cmd == NULL) { return -1; }
    len = strlen(cmd);
    if (len == 0U) { return -2; }
    if (len >= ETH_CMD_MAX_LEN)
    {
        c->dropped_cmd_too_long++;
        EventLog_Add(EVLOG_CMD_DROP, EVLOG_SRC_CLIENT(id), 1U);
        ClientHandler_AddRecvCredit(id, (uint32_t)len);
        LOG_DBG("[CLIENT] id=%u ch%u ERROR: cmd too long (len=%lu >= %u), dropped, total=%lu\r\n",
                        (unsigned)id, (unsigned)c->channel_id,
                        (unsigned long)len, (unsigned)ETH_CMD_MAX_LEN,
                        (unsigned long)c->dropped_cmd_too_long);
        return -2;
    }

    osKernelLock();

    if (ClientHandler_RxRingFree(id) < len)
    {
        /* По построению недостижимо (кольцо >= TCP_WND). Если всё же
         * случилось -- это баг в расчёте, печатаем каждый раз. */
        c->recv_credit += (uint32_t)len;
        osKernelUnlock();
        c->dropped_rx_cmd_ring++;
        EventLog_Add(EVLOG_CMD_DROP, EVLOG_SRC_CLIENT(id), 1U);
        LOG_DBG("[CLIENT] id=%u ch%u BUG: RX cmd ring full, cmd dropped, total=%lu used=%lu free=%lu\r\n",
                        (unsigned)id, (unsigned)c->channel_id,
                        (unsigned long)c->dropped_rx_cmd_ring,
                        (unsigned long)ClientHandler_RxRingUsed(id),
                        (unsigned long)ClientHandler_RxRingFree(id));
        return -3;
    }

    /* Только байты команды, без разделителя: команда и так заканчивается
     * на '\r' (ClientHandler_InputBytes кладёт сюда только полные строки),
     * по нему ClientHandler_PopCmdFromRxRing и найдёт её конец. */
    for (size_t i = 0; i < len; i++) { ClientHandler_RxRingWriteByte(id, (uint8_t)cmd[i]); }

    osKernelUnlock();
    return 0;
}

/* Извлекает одну команду из кольца: все байты до '\r' включительно (сам
 * '\r' остаётся частью команды -- CoreTask сравнивает с "N\r", "V\r" и
 * т.п.). Байты команды кредитуются для tcp_recved() -- место в кольце
 * освободилось. */
static int ClientHandler_PopCmdFromRxRing(uint8_t id, eth_cmd_msg_t *out_msg)
{
    uint32_t used;
    uint32_t temp_tail;
    uint32_t cmd_len = 0;
    uint8_t  found_cr = 0;
    client_slot_t *c = &g_clients[id];

    if (out_msg == NULL) { return -1; }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->client_id  = id;
    out_msg->channel_id = c->channel_id;   /* прописываем канал этого клиента в саму команду */

    osKernelLock();

    used = ClientHandler_RxRingUsed(id);
    if (used == 0U) { osKernelUnlock(); return -2; }

    /* Сначала только смотрим (tail не двигаем): есть ли в кольце полная
     * команда. ClientHandler_PushCmdToRxRing кладёт только команды
     * короче ETH_CMD_MAX_LEN, поэтому '\r' обязан найтись в пределах
     * ETH_CMD_MAX_LEN - 1 байт. */
    temp_tail = c->rx_cmd_tail;

    for (uint32_t i = 0; (i < used) && (i < (ETH_CMD_MAX_LEN - 1U)); i++)
    {
        uint8_t b = c->rx_cmd_ring[temp_tail];
        temp_tail = (temp_tail + 1U) % CLIENT_RX_CMD_RING_SIZE;
        cmd_len++;

        if (b == (uint8_t)'\r') { found_cr = 1U; break; }
    }

    if (!found_cr) { osKernelUnlock(); return -3; }

    for (uint32_t i = 0; i < cmd_len; i++)
    {
        out_msg->data[i] = (char)ClientHandler_RxRingReadByte(id);
    }
    out_msg->data[cmd_len] = '\0';

    c->recv_credit += cmd_len;   /* ровно столько байт TCP-потока ушло из кольца */

    osKernelUnlock();
    return 0;
}

static void ClientHandler_DrainRxCmdsToCore(uint8_t id)
{
    client_slot_t *c = &g_clients[id];
    uint32_t moved = 0;

    for (;;)
    {
        eth_cmd_msg_t msg;
        osStatus_t st;

        if (moved >= CLIENT_RX_CMD_DRAIN_LIMIT) { break; }

        if (c->pending_cmd_valid)
        {
            msg = c->pending_cmd;
            c->pending_cmd_valid = 0;
        }
        else
        {
            if (ClientHandler_PopCmdFromRxRing(id, &msg) != 0) { break; }
        }

        st = osMessageQueuePut(eth_to_core_queue, &msg, 0, 0);

        if (st != osOK)
        {
            c->pending_cmd = msg;
            c->pending_cmd_valid = 1;
            c->dropped_eth_to_core++;

            /* Первое ожидание -- сразу в лог, дальше не чаще раза в 1000 */
            if ((c->dropped_eth_to_core == 1U) || ((c->dropped_eth_to_core % 1000U) == 0U))
            {
                LOG_DBG("[CLIENT] id=%u ch%u eth_to_core full count=%lu rx_used=%lu\r\n",
                                (unsigned)id, (unsigned)c->channel_id,
                                (unsigned long)c->dropped_eth_to_core,
                                (unsigned long)ClientHandler_RxRingUsed(id));
            }
            break;
        }

        moved++;
    }
}

uint32_t ClientHandler_RxRingFreeBytes(uint8_t client_id)
{
    uint32_t free_bytes;

    if (client_id >= MAX_CLIENTS)        { return 0; }
    if (!g_clients[client_id].in_use)    { return 0; }

    osKernelLock();
    free_bytes = ClientHandler_RxRingFree(client_id);
    osKernelUnlock();

    return free_bytes;
}

/* Вызывается из tcpip-потока (raw_tcp_server.c, recv-колбэк) для КАЖДОГО
 * пришедшего байта -- без каких-либо проверок места: место гарантировано
 * тем, что неподтверждённых байт не больше TCP_WND. */
void ClientHandler_InputBytes(uint8_t client_id, const uint8_t *data, size_t len)
{
    client_slot_t *c;

    if (client_id >= MAX_CLIENTS) { return; }
    c = &g_clients[client_id];
    if (!c->in_use) { return; }
    if (data == NULL) { return; }

    for (size_t i = 0; i < len; i++)
    {
        uint8_t b = data[i];

        if (c->rx_line_pos < (CLIENT_RX_BUFFER_SIZE - 1U))
        {
            c->rx_line_buf[c->rx_line_pos++] = (char)b;
        }
        else
        {
            /* Строка длиннее буфера -- отбрасываем накопленное и текущий
             * байт. Их тоже кредитуем, иначе окно "утечёт". */
            c->dropped_line_too_long++;
            EventLog_Add(EVLOG_CMD_DROP, EVLOG_SRC_CLIENT(client_id), 1U);
            ClientHandler_AddRecvCredit(client_id, (uint32_t)c->rx_line_pos + 1U);
            LOG_DBG("[CLIENT] id=%u ch%u ERROR: input line too long (>%u bytes), "
                            "line dropped, total=%lu\r\n",
                            (unsigned)client_id, (unsigned)c->channel_id,
                            (unsigned)(CLIENT_RX_BUFFER_SIZE - 1U),
                            (unsigned long)c->dropped_line_too_long);
            c->rx_line_pos = 0;
            continue;
        }

        if (b == '\r')
        {
            c->rx_line_buf[c->rx_line_pos] = '\0';
            (void)ClientHandler_PushCmdToRxRing(client_id, c->rx_line_buf);
            c->rx_line_pos = 0;
        }
    }
}

/*
 * Пытается протолкнуть то, что накопилось в c->tx_batch, в TX-кольцо
 * платы (raw_tcp_server.c: RawTcpServer_SendAsync -> RawTcp_TxPush).
 * При неудаче батч НЕ обнуляется -- остаётся в tx_batch и будет повторно
 * предложен на следующем тике.
 *
 * Возвращает 1, если батч пуст или успешно протолкнут (tx_len = 0
 * после вызова), и 0, если протолкнуть не удалось (tx_len не менялся).
 */
static int ClientHandler_FlushTxBatch(client_slot_t *c, uint8_t client_id)
{
    int send_rc;

    if (c->tx_len == 0U) { return 1; }

#if CLIENT_USE_FAKE_SOURCE
    LOG_DBG("[CLIENT] id=%u TX->FAKE_CLIENT batch len=%u\r\n",
                    (unsigned)client_id, (unsigned)c->tx_len);
    c->tx_len = 0;
    return 1;
#else
    if (!RawTcpServer_HasClient(client_id))
    {
        /* клиента уже нет -- отправлять некому, отбрасываем осознанно */
        c->tx_len = 0;
        return 1;
    }

    send_rc = RawTcpServer_SendAsync(client_id, c->tx_batch, c->tx_len);
    if (send_rc != 0)
    {
        c->tcp_send_fail_count++;
        if ((c->tcp_send_fail_count == 1U) || ((c->tcp_send_fail_count % 1000U) == 0U))
        {
            LOG_DBG("[CLIENT] id=%u TCP send fails=%lu last_rc=%d "
                            "(TX-кольцо платы переполнено, батч НЕ потерян -- ждём следующего тика)\r\n",
                            (unsigned)client_id, (unsigned long)c->tcp_send_fail_count, send_rc);
        }
        return 0;   /* батч остаётся в tx_batch как есть */
    }

    c->tx_len = 0;
    return 1;
#endif
}

/*
 * Пристраивает отложенный pending_resp клиента в его tx_batch (при
 * необходимости сначала проталкивая батч). Возвращает 1, если отложенного
 * ответа нет или он успешно пристроен (можно дописывать новые), и 0, если
 * он всё ещё висит (новые ответы этого клиента дописывать НЕЛЬЗЯ -- они
 * обогнали бы отложенный).
 */
static int ClientHandler_TryPlacePending(client_slot_t *c, uint8_t client_id)
{
    size_t resp_len;

    if (!c->pending_resp_valid) { return 1; }

    resp_len = strlen(c->pending_resp.data);
    if ((resp_len == 0U) || (resp_len > CLIENT_TX_BATCH_SIZE))
    {
        c->pending_resp_valid = 0;
        return 1;
    }

    if ((c->tx_len + resp_len) > CLIENT_TX_BATCH_SIZE)
    {
        if (!ClientHandler_FlushTxBatch(c, client_id)) { return 0; }
    }

    memcpy(&c->tx_batch[c->tx_len], c->pending_resp.data, resp_len);
    c->tx_len += resp_len;
    c->pending_resp_valid = 0;
    return 1;
}

void ClientHandler_PollTx(void)
{
    uint32_t drained = 0;

    /* 1. Сначала пристраиваем застрявшее с прошлого тика -- оно старше
     *    всего, что сейчас лежит в core_to_eth_queue. */
    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        client_slot_t *c = &g_clients[id];
        if (!c->in_use) { continue; }
        (void)ClientHandler_TryPlacePending(c, id);
    }

    /* 2. Раскладываем новые ответы по клиентам. */
    for (;;)
    {
        eth_resp_msg_t resp;
        osStatus_t st;
        size_t resp_len;
        client_slot_t *c;

        if (drained >= CLIENT_TX_DRAIN_LIMIT) { break; }

        st = osMessageQueueGet(core_to_eth_queue, &resp, NULL, 0);
        if (st != osOK) { break; }

        drained++;

        if (resp.client_id >= MAX_CLIENTS) { continue; }
        c = &g_clients[resp.client_id];
        if (!c->in_use) { continue; }

        resp_len = strlen(resp.data);
        if (resp_len == 0U) { continue; }

        /* Пока у клиента висит более старый ответ, этот новый некуда
         * поставить, не нарушив порядок -- это реальная потеря (TX к ПК
         * застрял настолько, что не влезает даже один отложенный). */
        if (!ClientHandler_TryPlacePending(c, resp.client_id))
        {
            c->dropped_resp_overflow++;
            EventLog_Add(EVLOG_RESP_DROP, EVLOG_SRC_CLIENT(resp.client_id), 1U);
            if ((c->dropped_resp_overflow == 1U) || ((c->dropped_resp_overflow % 1000U) == 0U))
            {
                LOG_DBG("[CLIENT] id=%u ERROR: resp overflow (TX к ПК застрял), dropped total=%lu\r\n",
                                (unsigned)resp.client_id,
                                (unsigned long)c->dropped_resp_overflow);
            }
            continue;
        }

        if ((c->tx_len + resp_len) > CLIENT_TX_BATCH_SIZE)
        {
            if (!ClientHandler_FlushTxBatch(c, resp.client_id))
            {
                /* Батч не протолкнулся -- текущий ответ ещё не потерян,
                 * откладываем его (pending_resp гарантированно свободен:
                 * TryPlacePending выше вернул 1). */
                c->pending_resp = resp;
                c->pending_resp_valid = 1;
                continue;
            }
        }

        memcpy(&c->tx_batch[c->tx_len], resp.data, resp_len);
        c->tx_len += resp_len;
    }

    /* 3. Проталкиваем накопленное; если батч ушёл, а отложенный ответ
     *    остался -- пристраиваем и его и проталкиваем ещё раз. */
    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        client_slot_t *c = &g_clients[id];
        if (!c->in_use) { continue; }

        if (ClientHandler_FlushTxBatch(c, id) && c->pending_resp_valid)
        {
            if (ClientHandler_TryPlacePending(c, id))
            {
                (void)ClientHandler_FlushTxBatch(c, id);
            }
        }
    }
}

void ClientHandler_ClientConnected(uint8_t client_id, uint8_t channel_id)
{
    client_slot_t *c;

    if (client_id >= MAX_CLIENTS) { return; }

    c = &g_clients[client_id];

    osKernelLock();
    c->rx_cmd_head = 0;
    c->rx_cmd_tail = 0;
    c->recv_credit = 0;
    osKernelUnlock();

    c->rx_line_pos           = 0;
    c->pending_cmd_valid     = 0;
    c->pending_resp_valid    = 0;
    c->tx_len                = 0;
    c->channel_id            = channel_id;
    c->dropped_rx_cmd_ring   = 0;
    c->dropped_eth_to_core   = 0;
    c->dropped_cmd_too_long  = 0;
    c->dropped_line_too_long = 0;
    c->dropped_resp_overflow = 0;
    c->tcp_send_fail_count   = 0;
    c->in_use                = 1;

    LOG_INFO("[CLIENT] id=%u ch%u connected, slot reset\r\n",
                    (unsigned)client_id, (unsigned)channel_id);
}

void ClientHandler_ClientDisconnected(uint8_t client_id)
{
    client_slot_t *c;

    if (client_id >= MAX_CLIENTS) { return; }

    c = &g_clients[client_id];

    osKernelLock();
    c->rx_cmd_head = 0;
    c->rx_cmd_tail = 0;
    c->recv_credit = 0;
    osKernelUnlock();

    c->rx_line_pos        = 0;
    c->pending_cmd_valid  = 0;
    c->pending_resp_valid = 0;
    c->tx_len             = 0;
    c->in_use             = 0;

    LOG_INFO("[CLIENT] id=%u disconnected, slot freed\r\n", (unsigned)client_id);
    LOG_INFO("[CLIENT] id=%u drop stats: rx_cmd_ring=%lu eth_to_core_waits=%lu "
                    "cmd_too_long=%lu line_too_long=%lu tcp_send_fail=%lu resp_overflow=%lu\r\n",
                    (unsigned)client_id,
                    (unsigned long)c->dropped_rx_cmd_ring,
                    (unsigned long)c->dropped_eth_to_core,
                    (unsigned long)c->dropped_cmd_too_long,
                    (unsigned long)c->dropped_line_too_long,
                    (unsigned long)c->tcp_send_fail_count,
                    (unsigned long)c->dropped_resp_overflow);
}

static void ClientHandlerTask(void *argument)
{
    (void)argument;

    LOG_INFO("[CLIENT] ClientHandlerTask started\r\n");
    LOG_INFO("[CLIENT] RX cmd ring size per client=%lu bytes, TCP_WND=%lu, MAX_CLIENTS=%u\r\n",
                    (unsigned long)CLIENT_RX_CMD_RING_SIZE, (unsigned long)TCP_WND, (unsigned)MAX_CLIENTS);

#if CLIENT_USE_FAKE_SOURCE
    FakeClientSource_Init();
#endif

    for (;;)
    {
#if CLIENT_USE_FAKE_SOURCE
        FakeClientSource_Poll();
#endif

        for (uint8_t id = 0; id < MAX_CLIENTS; id++)
        {
            if (!g_clients[id].in_use) { continue; }

            for (uint32_t pass = 0; pass < 64U; pass++)
            {
                uint32_t before = ClientHandler_RxRingUsed(id);
                ClientHandler_DrainRxCmdsToCore(id);
                if (ClientHandler_RxRingUsed(id) == before) { break; }
            }
        }

        ClientHandler_PollTx();

        /* Команды ушли из кольца дальше по конвейеру -- просим tcpip-поток
         * подтвердить их байты (tcp_recved), это и открывает TCP-окно. */
        if (ClientHandler_AnyRecvCredit())
        {
            RawTcpServer_KickRecved();
        }

        osDelay(1);
    }
}

void ClientHandlerTask_Start(void)
{
    const osThreadAttr_t attr = {
        .name = "ClientHandler",
        .stack_size = 8192,
        .priority = (osPriority_t)osPriorityNormal
    };

    clientHandlerTaskHandle = osThreadNew(ClientHandlerTask, NULL, &attr);

    if (!clientHandlerTaskHandle)
    {
        LOG_ERR("[CLIENT] ERROR: task create failed\r\n");
    }
    else
    {
        LOG_INFO("[CLIENT] task created\r\n");
    }
}

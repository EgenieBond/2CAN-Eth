/*
 * client_handler.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *  Обновлено: поддержка ДВУХ независимых физических CAN-каналов --
 *  каждый слот клиента теперь помнит свой channel_id (задаётся один
 *  раз при подключении), и этот channel_id прописывается в каждую
 *  команду (eth_cmd_msg_t), уходящую в CoreTask -- чтобы там точно
 *  знали, к какому физическому каналу относится команда данного
 *  клиента.
 *
 *  core_to_eth_queue остаётся ОБЩЕЙ на всю систему (оба канала) --
 *  ClientHandler_PollTx() разбирает её целиком за один проход и
 *  раскладывает сообщения по персональным TX-буферам клиентов, ровно
 *  как раньше (это не зависит от того, к какому каналу привязан
 *  клиент -- то, что channel_id прописан в ответе, важно только для
 *  диагностики/логов, доставка всё равно идёт по client_id).
 *
 *  Обновлено ещё раз (разбор стопора на burst >= ~310-320 кадров):
 *  найден и исправлен баг потери данных в ClientHandler_PollTx() --
 *  см. подробный комментарий над ClientHandler_FlushTxBatch() и в
 *  самой функции. Коротко: при массовом burst'е TX-кольцо клиента в
 *  raw_tcp_server.c (RAW_TCP_TX_RING_SIZE_PER_CLIENT) может на
 *  короткое время переполниться -- RawTcpServer_SendAsync() в этом
 *  случае возвращал ошибку, а ClientHandler_PollTx() эту ошибку
 *  просто считал (tcp_send_fail_count++) и БЕЗУСЛОВНО обнулял
 *  c->tx_len, то есть выбрасывал уже собранный батч ответов НАВСЕГДА.
 *  При этом порог печати в лог (раз в 100) почти никогда не
 *  срабатывал на единичных потерях (7-8 кадров из burst'а в 320) --
 *  поэтому в UART-логе всё выглядело так, будто потерь нигде нет.
 *  Теперь непротолкнутый батч не обнуляется, а остаётся в tx_batch и
 *  повторно пробуется на следующем тике (ClientHandlerTask крутится
 *  каждую 1 мс) -- ровно тот же приём отложенного элемента, что уже
 *  был у pending_cmd/pending_cmd_valid на RX-стороне.
 */

#include "client_handler.h"
#include "cmsis_os.h"
#include "app_queues.h"
#include "debug_uart.h"
#include "fake_client_source.h"
#include "raw_tcp_server.h"

#include <string.h>
#include <stdint.h>

#define CLIENT_RX_BUFFER_SIZE        128
// #define CLIENT_RX_CMD_RING_SIZE      (8U * 1024U)
#define CLIENT_RX_CMD_RING_SIZE      (11U * 1024U)
#define CLIENT_RX_CMD_DRAIN_LIMIT    256
#define CLIENT_TX_BATCH_SIZE         1024
#define CLIENT_TX_DRAIN_LIMIT        1024

#define CLIENT_USE_FAKE_SOURCE       0

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

    eth_cmd_msg_t pending_cmd;
    uint8_t       pending_cmd_valid;

    /* --- передача: core_to_eth_queue -> TX-буфер -> TCP --- */
    uint8_t  tx_batch[CLIENT_TX_BATCH_SIZE];
    size_t   tx_len;

    /* Один отложенный resp -- если TX-кольцо платы (raw_tcp_server.c)
     * оказалось временно переполнено и RawTcpServer_SendAsync() не
     * принял батч, а в очереди уже ждал СЛЕДУЮЩИЙ resp именно этого
     * клиента. Держим его тут, чтобы не терять и не ломать порядок
     * байт на линии -- см. ClientHandler_PollTx(). */
    eth_resp_msg_t pending_resp;
    uint8_t        pending_resp_valid;

    /* --- диагностика --- */
    uint32_t dropped_rx_cmd_ring;
    uint32_t dropped_eth_to_core;
    uint32_t tcp_send_fail_count;
    uint32_t dropped_cmd_too_long;     /* ClientHandler_PushCmdToRxRing: len >= ETH_CMD_MAX_LEN */
    uint32_t dropped_line_too_long;    /* ClientHandler_InputBytes: строка длиннее rx_line_buf */
    uint32_t dropped_resp_overflow;    /* два resp подряд не влезли, пока первый ещё pending -- практически недостижимо, см. ClientHandler_PollTx() */
} client_slot_t;

static client_slot_t g_clients[MAX_CLIENTS];

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
        DebugUART_Print("[CLIENT] id=%u ch%u ERROR: cmd too long (len=%lu >= %u), dropped, total=%lu\r\n",
                        (unsigned)id, (unsigned)c->channel_id,
                        (unsigned long)len, (unsigned)ETH_CMD_MAX_LEN,
                        (unsigned long)c->dropped_cmd_too_long);
        return -2;
    }

    osKernelLock();

    if (ClientHandler_RxRingFree(id) < (len + 1U))
    {
        osKernelUnlock();
        c->dropped_rx_cmd_ring++;
        /* Первый же дроп печатаем сразу -- порог "раз в 100" удобен при
         * массовом переполнении, но единичную редкую потерю (ровно наш
         * случай -- 1 кадр за весь прогон) с таким порогом в лог
         * вообще никогда не попал бы. */
        if ((c->dropped_rx_cmd_ring == 1U) || ((c->dropped_rx_cmd_ring % 100U) == 0U))
        {
            DebugUART_Print("[CLIENT] id=%u ch%u RX cmd ring drops=%lu used=%lu free=%lu\r\n",
                            (unsigned)id, (unsigned)c->channel_id,
                            (unsigned long)c->dropped_rx_cmd_ring,
                            (unsigned long)ClientHandler_RxRingUsed(id),
                            (unsigned long)ClientHandler_RxRingFree(id));
        }
        return -3;
    }

    for (size_t i = 0; i < len; i++) { ClientHandler_RxRingWriteByte(id, (uint8_t)cmd[i]); }
    ClientHandler_RxRingWriteByte(id, 0U);

    osKernelUnlock();
    return 0;
}

static int ClientHandler_PopCmdFromRxRing(uint8_t id, eth_cmd_msg_t *out_msg)
{
    uint32_t used;
    uint32_t temp_tail;
    uint32_t count = 0;
    uint8_t found_zero = 0;
    client_slot_t *c = &g_clients[id];

    if (out_msg == NULL) { return -1; }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->client_id  = id;
    out_msg->channel_id = c->channel_id;   /* прописываем канал этого клиента в саму команду */

    osKernelLock();

    used = ClientHandler_RxRingUsed(id);
    if (used == 0U) { osKernelUnlock(); return -2; }

    temp_tail = c->rx_cmd_tail;

    for (uint32_t i = 0; i < used; i++)
    {
        uint8_t b = c->rx_cmd_ring[temp_tail];
        temp_tail = (temp_tail + 1U) % CLIENT_RX_CMD_RING_SIZE;

        if (b == 0U) { found_zero = 1U; break; }

        count++;
        if (count >= (ETH_CMD_MAX_LEN - 1U)) { break; }
    }

    if (!found_zero) { osKernelUnlock(); return -3; }

    for (uint32_t i = 0; i < (ETH_CMD_MAX_LEN - 1U); i++)
    {
        uint8_t b = ClientHandler_RxRingReadByte(id);
        if (b == 0U)
        {
            out_msg->data[i] = '\0';
            osKernelUnlock();
            return 0;
        }
        out_msg->data[i] = (char)b;
    }

    out_msg->data[ETH_CMD_MAX_LEN - 1U] = '\0';
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

            /* Та же логика: первая потеря -- сразу в лог, дальше не
             * чаще раза в 1000, чтобы не захлёстывать UART при
             * длительном заторе. */
            if ((c->dropped_eth_to_core == 1U) || ((c->dropped_eth_to_core % 1000U) == 0U))
            {
                DebugUART_Print("[CLIENT] id=%u ch%u eth_to_core full count=%lu rx_used=%lu\r\n",
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
            c->dropped_line_too_long++;
            DebugUART_Print("[CLIENT] id=%u ch%u ERROR: input line too long (>%u bytes), "
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
 *
 * ГЛАВНОЕ ОТЛИЧИЕ от старой версии: при неудаче (кольцо клиента
 * временно переполнено -- бывает при большом burst'е, когда за один
 * проход ClientHandler_PollTx() успевает надробить и протолкнуть
 * несколько батчей по 1024 байта подряд, а TCP-кольцо клиента не
 * успевает освободиться между ними) батч НЕ обнуляется и НЕ
 * выбрасывается -- он остаётся в tx_batch как есть и будет повторно
 * предложен на следующем тике (ClientHandlerTask крутится каждую
 * 1 мс, а значит и RawTcp_TryFlush успевает освободить место). Именно
 * отсюда раньше терялись кадры при burst >= ~310-320: старая версия
 * при неудаче просто делала c->tx_len = 0, выкидывая уже готовый к
 * отправке батч навсегда, а счётчик неудач печатался в лог только
 * раз в 100 -- единичные потери (7-8 из 320) туда никогда не попадали.
 *
 * Возвращает 1, если батч пуст или успешно протолкнут (tx_len = 0
 * после вызова), и 0, если протолкнуть не удалось (tx_len не менялся,
 * нужно повторить попытку позже).
 */
static int ClientHandler_FlushTxBatch(client_slot_t *c, uint8_t client_id)
{
    int send_rc;

    if (c->tx_len == 0U) { return 1; }

#if CLIENT_USE_FAKE_SOURCE
    DebugUART_Print("[CLIENT] id=%u TX->FAKE_CLIENT batch len=%u\r\n",
                    (unsigned)client_id, (unsigned)c->tx_len);
    c->tx_len = 0;
    return 1;
#else
    if (!RawTcpServer_HasClient(client_id))
    {
        /* клиента уже нет -- отправлять некому, эти байты теперь
         * действительно никому не нужны, отбрасываем осознанно */
        c->tx_len = 0;
        return 1;
    }

    send_rc = RawTcpServer_SendAsync(client_id, c->tx_batch, c->tx_len);
    if (send_rc != 0)
    {
        c->tcp_send_fail_count++;
        if ((c->tcp_send_fail_count == 1U) || ((c->tcp_send_fail_count % 100U) == 0U))
        {
            DebugUART_Print("[CLIENT] id=%u TCP send fails=%lu last_rc=%d "
                            "(TX-кольцо платы переполнено, батч НЕ потерян -- ждём следующего тика)\r\n",
                            (unsigned)client_id, (unsigned long)c->tcp_send_fail_count, send_rc);
        }
        return 0;   /* батч остаётся в tx_batch как есть -- попробуем снова через ~1 мс */
    }

    c->tx_len = 0;
    return 1;
#endif
}

void ClientHandler_PollTx(void)
{
    uint32_t drained = 0;

    /* Сначала -- если на прошлом тике для какого-то клиента ответ не
     * удалось протолкнуть в TX-кольцо (оно было переполнено), пробуем
     * пристроить застрявшее СНАЧАЛА: иначе нарушится порядок байт на
     * линии (новый resp того же клиента не может обойти застрявший
     * предыдущий). */
    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        client_slot_t *c = &g_clients[id];
        size_t resp_len;

        if (!c->pending_resp_valid) { continue; }
        if (!ClientHandler_FlushTxBatch(c, id)) { continue; }  /* ещё не освободилось -- попробуем на следующем тике */

        resp_len = strlen(c->pending_resp.data);
        if ((resp_len > 0U) && (resp_len <= CLIENT_TX_BATCH_SIZE))
        {
            memcpy(&c->tx_batch[c->tx_len], c->pending_resp.data, resp_len);
            c->tx_len += resp_len;
        }
        c->pending_resp_valid = 0;
    }

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

        if ((c->tx_len + resp_len) > CLIENT_TX_BATCH_SIZE)
        {
            if (!ClientHandler_FlushTxBatch(c, resp.client_id))
            {
                /* Кольцо платы переполнено -- старый батч остаётся в
                 * tx_batch на следующий тик. Текущий resp при этом ЕЩЁ
                 * НЕ потерян (он только что вычитан из общей очереди) --
                 * кладём его в pending_resp этого клиента и идём дальше
                 * по очереди: другие клиенты не должны ждать из-за
                 * одного застрявшего. */
                if (!c->pending_resp_valid)
                {
                    c->pending_resp = resp;
                    c->pending_resp_valid = 1;
                }
                else
                {
                    /* Два подряд застрявших resp у одного клиента --
                     * практически недостижимо (TCP ACK возвращается за
                     * доли мс, тик ClientHandlerTask -- 1 мс), но если
                     * всё же случилось -- не молчим, как раньше, а сразу
                     * видно в логе. */
                    c->dropped_resp_overflow++;
                    DebugUART_Print("[CLIENT] id=%u ERROR: resp overflow "
                                    "(два подряд не влезли), dropped total=%lu\r\n",
                                    (unsigned)resp.client_id,
                                    (unsigned long)c->dropped_resp_overflow);
                }
                continue;
            }
        }

        memcpy(&c->tx_batch[c->tx_len], resp.data, resp_len);
        c->tx_len += resp_len;
    }

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        client_slot_t *c = &g_clients[id];
        if (!c->in_use) { continue; }
        (void)ClientHandler_FlushTxBatch(c, id);
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
    osKernelUnlock();

    c->rx_line_pos          = 0;
    c->pending_cmd_valid    = 0;
    c->pending_resp_valid   = 0;
    c->tx_len               = 0;
    c->channel_id           = channel_id;
    c->dropped_rx_cmd_ring   = 0;
    c->dropped_eth_to_core   = 0;
    c->dropped_cmd_too_long  = 0;
    c->dropped_line_too_long = 0;
    c->dropped_resp_overflow = 0;
    c->tcp_send_fail_count   = 0;
    c->in_use                = 1;

    DebugUART_Print("[CLIENT] id=%u ch%u connected, slot reset\r\n",
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
    osKernelUnlock();

    c->rx_line_pos        = 0;
    c->pending_cmd_valid  = 0;
    c->pending_resp_valid = 0;
    c->tx_len             = 0;
    c->in_use             = 0;

    DebugUART_Print("[CLIENT] id=%u disconnected, slot freed\r\n", (unsigned)client_id);
    DebugUART_Print("[CLIENT] id=%u drop stats: rx_cmd_ring=%lu eth_to_core=%lu "
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

    DebugUART_Print("[CLIENT] ClientHandlerTask started\r\n");
    DebugUART_Print("[CLIENT] RX cmd ring size per client=%lu bytes, MAX_CLIENTS=%u\r\n",
                    (unsigned long)CLIENT_RX_CMD_RING_SIZE, (unsigned)MAX_CLIENTS);

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
        DebugUART_Print("[CLIENT] ERROR: task create failed\r\n");
    }
    else
    {
        DebugUART_Print("[CLIENT] task created\r\n");
    }
}

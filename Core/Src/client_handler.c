/*
 * client_handler.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
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

/*
 * Внутренний RX ring buffer для готовых SLCAN-команд.
 *
 * Зачем нужен:
 * TCP может быстро прислать сразу 1000/5000/10000 команд.
 * Маленькая eth_to_core_queue не успевает их принять.
 * Поэтому сначала складываем команды сюда, а потом ClientHandlerTask
 * постепенно перекладывает их в eth_to_core_queue.
 */
#define CLIENT_RX_CMD_RING_SIZE      (32U * 1024U)

/*
 * Сколько команд максимум за один цикл перекладываем
 * из внутреннего RX ring в eth_to_core_queue.
 */
#define CLIENT_RX_CMD_DRAIN_LIMIT    256

/*
 * Размер одной TCP-пачки ответов.
 */
#define CLIENT_TX_BATCH_SIZE         1024

/*
 * Сколько сообщений максимум склеиваем в одну TCP-пачку.
 */
#define CLIENT_TX_MAX_MSG_PER_BATCH  256

/*
 * Сколько TCP-пачек максимум отправляем за один вызов ClientHandler_PollTx().
 */
#define CLIENT_TX_MAX_BATCH_PER_POLL 1

#define CLIENT_USE_FAKE_SOURCE       0

static osThreadId_t clientHandlerTaskHandle = NULL;

static char rx_line_buf[CLIENT_RX_BUFFER_SIZE];
static size_t rx_line_pos = 0;

/* RX command ring */
static uint8_t g_rx_cmd_ring[CLIENT_RX_CMD_RING_SIZE];
static volatile uint32_t g_rx_cmd_head = 0;
static volatile uint32_t g_rx_cmd_tail = 0;

static eth_cmd_msg_t pending_cmd;
static uint8_t pending_cmd_valid = 0;

static eth_resp_msg_t pending_resp;
static uint8_t pending_resp_valid = 0;

static uint32_t dropped_rx_cmd_ring = 0;
static uint32_t dropped_eth_to_core = 0;
static uint32_t tcp_send_fail_count = 0;

static uint32_t ClientHandler_RxRingUsed(void)
{
    if (g_rx_cmd_head >= g_rx_cmd_tail)
    {
        return g_rx_cmd_head - g_rx_cmd_tail;
    }

    return CLIENT_RX_CMD_RING_SIZE - g_rx_cmd_tail + g_rx_cmd_head;
}

static uint32_t ClientHandler_RxRingFree(void)
{
    return CLIENT_RX_CMD_RING_SIZE - ClientHandler_RxRingUsed() - 1U;
}

static void ClientHandler_RxRingWriteByte(uint8_t b)
{
    g_rx_cmd_ring[g_rx_cmd_head] = b;
    g_rx_cmd_head = (g_rx_cmd_head + 1U) % CLIENT_RX_CMD_RING_SIZE;
}

static uint8_t ClientHandler_RxRingReadByte(void)
{
    uint8_t b = g_rx_cmd_ring[g_rx_cmd_tail];
    g_rx_cmd_tail = (g_rx_cmd_tail + 1U) % CLIENT_RX_CMD_RING_SIZE;
    return b;
}

static int ClientHandler_PushCmdToRxRing(const char *cmd)
{
    size_t len;

    if (cmd == NULL)
    {
        return -1;
    }

    len = strlen(cmd);

    if ((len == 0U) || (len >= ETH_CMD_MAX_LEN))
    {
        return -2;
    }

    /*
     * Кладём команду вместе с завершающим '\0'.
     * В ring будет: 't' '1' '2' '3' ... '\r' '\0'
     */
    osKernelLock();

    if (ClientHandler_RxRingFree() < (len + 1U))
    {
        osKernelUnlock();

        dropped_rx_cmd_ring++;

        if ((dropped_rx_cmd_ring % 100U) == 0U)
        {
            DebugUART_Print("[CLIENT] RX cmd ring drops=%lu used=%lu free=%lu\r\n",
                            (unsigned long)dropped_rx_cmd_ring,
                            (unsigned long)ClientHandler_RxRingUsed(),
                            (unsigned long)ClientHandler_RxRingFree());
        }

        return -3;
    }

    for (size_t i = 0; i < len; i++)
    {
        ClientHandler_RxRingWriteByte((uint8_t)cmd[i]);
    }

    ClientHandler_RxRingWriteByte(0U);

    osKernelUnlock();

    return 0;
}

static int ClientHandler_PopCmdFromRxRing(eth_cmd_msg_t *out_msg)
{
    uint32_t used;
    uint32_t temp_tail;
    uint32_t count = 0;
    uint8_t found_zero = 0;

    if (out_msg == NULL)
    {
        return -1;
    }

    memset(out_msg, 0, sizeof(*out_msg));

    osKernelLock();

    used = ClientHandler_RxRingUsed();

    if (used == 0U)
    {
        osKernelUnlock();
        return -2;
    }

    /*
     * Сначала проверяем, есть ли в ring целая команда до '\0'.
     * Если целой команды ещё нет — ничего не вытаскиваем.
     */
    temp_tail = g_rx_cmd_tail;

    for (uint32_t i = 0; i < used; i++)
    {
        uint8_t b = g_rx_cmd_ring[temp_tail];

        temp_tail = (temp_tail + 1U) % CLIENT_RX_CMD_RING_SIZE;

        if (b == 0U)
        {
            found_zero = 1U;
            break;
        }

        count++;

        if (count >= (ETH_CMD_MAX_LEN - 1U))
        {
            break;
        }
    }

    if (!found_zero)
    {
        osKernelUnlock();
        return -3;
    }

    /*
     * Теперь реально читаем команду.
     */
    for (uint32_t i = 0; i < (ETH_CMD_MAX_LEN - 1U); i++)
    {
        uint8_t b = ClientHandler_RxRingReadByte();

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

static void ClientHandler_DrainRxCmdsToCore(void)
{
    uint32_t moved = 0;

    for (;;)
    {
        eth_cmd_msg_t msg;
        osStatus_t st;

        if (moved >= CLIENT_RX_CMD_DRAIN_LIMIT)
        {
            break;
        }

        if (pending_cmd_valid)
        {
            msg = pending_cmd;
            pending_cmd_valid = 0;
        }
        else
        {
            if (ClientHandler_PopCmdFromRxRing(&msg) != 0)
            {
                break;
            }
        }

        st = osMessageQueuePut(eth_to_core_queue, &msg, 0, 0);

        if (st != osOK)
        {
            pending_cmd = msg;
            pending_cmd_valid = 1;

            dropped_eth_to_core++;

            if ((dropped_eth_to_core % 1000U) == 0U)
            {
                DebugUART_Print("[CLIENT] eth_to_core full count=%lu rx_used=%lu\r\n",
                                (unsigned long)dropped_eth_to_core,
                                (unsigned long)ClientHandler_RxRingUsed());
            }

            break;
        }

        moved++;
    }
}

void ClientHandler_InputBytes(const uint8_t *data, size_t len)
{
    if (data == NULL)
    {
        return;
    }

    for (size_t i = 0; i < len; i++)
    {
        uint8_t b = data[i];

        if (rx_line_pos < (CLIENT_RX_BUFFER_SIZE - 1U))
        {
            rx_line_buf[rx_line_pos++] = (char)b;
        }
        else
        {
            rx_line_pos = 0;
            continue;
        }

        if (b == '\r')
        {
            rx_line_buf[rx_line_pos] = '\0';

            /*
             * Теперь не кладём сразу в eth_to_core_queue.
             * Сначала кладём во внутренний RX ring buffer.
             */
            (void)ClientHandler_PushCmdToRxRing(rx_line_buf);

            rx_line_pos = 0;
        }
    }
}

static uint8_t ClientHandler_BuildTxBatch(uint8_t *tx_batch,
                                          size_t tx_batch_size,
                                          size_t *out_len)
{
    size_t tx_len = 0;
    uint32_t msg_count = 0;

    if ((tx_batch == NULL) || (out_len == NULL) || (tx_batch_size == 0U))
    {
        return 0;
    }

    *out_len = 0;

    for (;;)
    {
        eth_resp_msg_t resp;
        osStatus_t st;
        size_t resp_len;

        if (msg_count >= CLIENT_TX_MAX_MSG_PER_BATCH)
        {
            break;
        }

        if (pending_resp_valid)
        {
            resp = pending_resp;
            pending_resp_valid = 0;
        }
        else
        {
            st = osMessageQueueGet(core_to_eth_queue, &resp, NULL, 0);

            if (st != osOK)
            {
                break;
            }
        }

        resp_len = strlen(resp.data);

        if (resp_len == 0U)
        {
            continue;
        }

        if ((tx_len + resp_len) > tx_batch_size)
        {
            pending_resp = resp;
            pending_resp_valid = 1;
            break;
        }

        memcpy(&tx_batch[tx_len], resp.data, resp_len);
        tx_len += resp_len;
        msg_count++;
    }

    *out_len = tx_len;

    return (tx_len > 0U) ? 1U : 0U;
}

void ClientHandler_PollTx(void)
{
    uint8_t tx_batch[CLIENT_TX_BATCH_SIZE];

    for (uint32_t batch_i = 0; batch_i < CLIENT_TX_MAX_BATCH_PER_POLL; batch_i++)
    {
        size_t tx_len = 0;

        if (!ClientHandler_BuildTxBatch(tx_batch, sizeof(tx_batch), &tx_len))
        {
            return;
        }

#if CLIENT_USE_FAKE_SOURCE
        DebugUART_Print("[CLIENT] TX->FAKE_CLIENT batch len=%u\r\n",
                        (unsigned)tx_len);
#else
        if (!RawTcpServer_HasClient())
        {
            return;
        }

        int send_rc = RawTcpServer_SendAsync(tx_batch, tx_len);

        if (send_rc != 0)
        {
            tcp_send_fail_count++;

            if ((tcp_send_fail_count % 100U) == 0U)
            {
                DebugUART_Print("[CLIENT] TCP send fails=%lu last_rc=%d len=%u\r\n",
                                (unsigned long)tcp_send_fail_count,
                                send_rc,
                                (unsigned)tx_len);
            }

            return;
        }
#endif
    }
}

static void ClientHandlerTask(void *argument)
{
    (void)argument;

    DebugUART_Print("[CLIENT] ClientHandlerTask started\r\n");
    DebugUART_Print("[CLIENT] RX cmd ring size=%lu bytes\r\n",
                    (unsigned long)CLIENT_RX_CMD_RING_SIZE);

#if CLIENT_USE_FAKE_SOURCE
    FakeClientSource_Init();
#endif

    for (;;)
    {
#if CLIENT_USE_FAKE_SOURCE
        FakeClientSource_Poll();
#endif

        /*
         * Сначала разгружаем входной ring в Core.
         */
        ClientHandler_DrainRxCmdsToCore();

        /*
         * Потом забираем ответы Core -> TCP.
         */
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

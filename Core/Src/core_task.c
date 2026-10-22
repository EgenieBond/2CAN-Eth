/*
 * core_task.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *  Обновлено: поддержка ДВУХ независимых физических CAN-каналов.
 *
 *  Состояние клиента (g_client_can[]) теперь несёт channel_id --
 *  к какому физическому каналу привязан клиент (задаётся ОДИН РАЗ
 *  при подключении, по TCP-порту, на который он пришёл, и не
 *  меняется до отключения).
 *
 *  Арбитраж шины (g_bus) стал МАССИВОМ -- g_bus[NUM_CHANNELS] --
 *  у каждого канала СВОЙ независимый арбитраж битрейта/режима между
 *  клиентами, привязанными именно к этому каналу. Клиент канала 0
 *  никак не пересекается и не может конфликтовать с клиентом канала 1
 *  -- это и есть суть требования "каждому клиенту -- своя шина"
 *  (просто внутри одного канала по-прежнему может быть несколько
 *  клиентов, как раньше).
 *
 *  CoreTask в основном цикле теперь вычитывает ОБЕ пары CAN-очередей
 *  (can_to_core_queues[0] и [1]) по очереди, передавая channel_id
 *  в CoreTask_HandleCanRx, чтобы рассылка ответов ушла только тем
 *  клиентам, что привязаны именно к этому каналу.
 *
 *  Обновлено (flow control ETH -> CAN):
 *  - если core_to_can_queue полна, кадр больше НЕ выбрасывается с "\a":
 *    CoreTask держит команду у себя (pending_cmd) и перестаёт брать новые
 *    из eth_to_core_queue -- затор доходит до TCP-окна и тормозит ПК;
 *  - то же самое, если некуда положить ответ "Z\r" (core_to_eth_queue
 *    полна) -- иначе кадр ушёл бы на шину, а подтверждение потерялось;
 *  - CAN RX не вычитывается из can_to_core_queues, пока в core_to_eth_queue
 *    нет места под рассылку -- кадр ждёт в can_to_core_queue, а не
 *    теряется здесь (CoreTask -- единственный писатель core_to_eth_queue,
 *    поэтому проверка места заранее надёжна).
 */

#include "core_task.h"
#include "cmsis_os.h"
#include "app_queues.h"
#include "slcan_parser.h"
#include "slcan_types.h"
#include "debug_uart.h"
#include "can_task.h"

#include <string.h>
#include <stdio.h>

#define NETCAN_SERIAL_RESPONSE   "N210109796\r"
#define NETCAN_VERSION_RESPONSE  "V1017\r"

#define CORE_SLCAN_STRESS_TEST_NO_CAN 0

static osThreadId_t coreTaskHandle = NULL;

typedef struct
{
    uint8_t         channel_id;    /* канал, к которому привязан клиент -- задаётся один раз при подключении */
    uint8_t         bitrate_code;
    uint32_t        bitrate_bps;
    core_can_mode_t mode;           /* CLOSED, пока клиент сам не открыл канал */
} client_can_state_t;

static client_can_state_t g_client_can[MAX_CLIENTS];

typedef struct
{
    uint8_t         open_client_count;
    core_can_mode_t mode;
    uint32_t        bitrate_bps;
} can_bus_state_t;

static can_bus_state_t g_bus[NUM_CHANNELS];

typedef enum
{
    CORE_CMD_DONE  = 0,   /* команда обработана, ответ отправлен */
    CORE_CMD_RETRY = 1    /* некуда положить кадр/ответ -- ничего не сделано, повторить позже */
} core_cmd_result_t;

/* Сколько максимум держим одну команду в ожидании места.
 * При нормальной перегрузке слот освобождается каждые ~0.2 мс (1 кадр на 1 Мбит/с),
 * так что 500 мс -- это уже не back-pressure, а авария шины.
 * (Предполагается configTICK_RATE_HZ = 1000, т.е. 1 тик = 1 мс.) */
#define CORE_PENDING_MAX_MS   500U

static uint32_t g_core_to_can_backpressure[NUM_CHANNELS];
static uint32_t g_core_to_eth_backpressure = 0;

static void CoreTask_PrintBitrate(const char *prefix, uint8_t bitrate_code, uint32_t bitrate_bps)
{
    if (bitrate_code != 0xFFU)
    {
        DebugUART_Print("%sS%u -> %lu bit/s\r\n", prefix, (unsigned)bitrate_code, (unsigned long)bitrate_bps);
    }
    else
    {
        DebugUART_Print("%sdirect %lu bit/s\r\n", prefix, (unsigned long)bitrate_bps);
    }
}

static void CoreTask_SendResponse(const eth_resp_msg_t *resp)
{
    if (osMessageQueuePut(core_to_eth_queue, resp, 0, 0) != osOK)
    {
        static uint32_t core_to_eth_full_count = 0;
        core_to_eth_full_count++;
        if ((core_to_eth_full_count == 1U) || ((core_to_eth_full_count % 200U) == 0U))
        {
            DebugUART_Print("[CORE] ERROR: core_to_eth_queue full (count=%lu)\r\n",
                            (unsigned long)core_to_eth_full_count);
        }
    }
}

static void CoreTask_SetFrameAck(const can_frame_t *frame, eth_resp_msg_t *resp)
{
    if ((frame->Flags & CAN_FLAG_EXTENDED) != 0U)
    {
        snprintf(resp->data, sizeof(resp->data), "Z\r");
    }
    else
    {
        snprintf(resp->data, sizeof(resp->data), "z\r");
    }
}

/*
 * Пытается открыть канал для клиента id в режиме desired_mode.
 * Использует g_client_can[id].channel_id, чтобы понять, с каким
 * физическим каналом (и с чьим арбитражом в g_bus[]) работать.
 */
static int CoreTask_TryOpenChannel(uint8_t id, core_can_mode_t desired_mode, eth_resp_msg_t *resp)
{
    client_can_state_t *cs = &g_client_can[id];
    uint8_t ch = cs->channel_id;

    if (cs->mode != CORE_CAN_MODE_CLOSED)
    {
        snprintf(resp->data, sizeof(resp->data), "\a");
        DebugUART_Print("[CORE] id=%u ch%u ERROR: channel already open by this client\r\n",
                        (unsigned)id, (unsigned)ch);
        return -1;
    }

#if CORE_SLCAN_STRESS_TEST_NO_CAN
    cs->mode = desired_mode;
    snprintf(resp->data, sizeof(resp->data), "\r");
    DebugUART_Print("[CORE] id=%u ch%u channel OPEN mode=%d (NO_CAN stress mode)\r\n",
                    (unsigned)id, (unsigned)ch, (int)desired_mode);
    return 0;
#else
    if (g_bus[ch].open_client_count == 0U)
    {
        if (CanTask_Open(ch, desired_mode, cs->bitrate_bps) != 0)
        {
            snprintf(resp->data, sizeof(resp->data), "\a");
            DebugUART_Print("[CORE] id=%u ch%u ERROR: failed to open CAN (first opener)\r\n",
                            (unsigned)id, (unsigned)ch);
            return -1;
        }

        g_bus[ch].mode = desired_mode;
        g_bus[ch].bitrate_bps = cs->bitrate_bps;
        g_bus[ch].open_client_count = 1U;
        cs->mode = desired_mode;

        snprintf(resp->data, sizeof(resp->data), "\r");
        DebugUART_Print("[CORE] id=%u ch%u channel OPEN mode=%d -- physically started bus\r\n",
                        (unsigned)id, (unsigned)ch, (int)desired_mode);
        CoreTask_PrintBitrate("[CORE] active bitrate: ", cs->bitrate_code, cs->bitrate_bps);
        return 0;
    }
    else
    {
        if ((g_bus[ch].mode != desired_mode) || (g_bus[ch].bitrate_bps != cs->bitrate_bps))
        {
            snprintf(resp->data, sizeof(resp->data), "\a");
            DebugUART_Print("[CORE] id=%u ch%u ERROR: bus busy with mode=%d bitrate=%lu, "
                            "requested mode=%d bitrate=%lu -- conflict\r\n",
                            (unsigned)id, (unsigned)ch,
                            (int)g_bus[ch].mode, (unsigned long)g_bus[ch].bitrate_bps,
                            (int)desired_mode, (unsigned long)cs->bitrate_bps);
            return -1;
        }

        g_bus[ch].open_client_count++;
        cs->mode = desired_mode;

        snprintf(resp->data, sizeof(resp->data), "\r");
        DebugUART_Print("[CORE] id=%u ch%u channel OPEN mode=%d -- joined existing bus (open_count=%u)\r\n",
                        (unsigned)id, (unsigned)ch, (int)desired_mode,
                        (unsigned)g_bus[ch].open_client_count);
        return 0;
    }
#endif
}

static void CoreTask_CloseChannel(uint8_t id, eth_resp_msg_t *resp)
{
    client_can_state_t *cs = &g_client_can[id];
    uint8_t ch = cs->channel_id;

    if (cs->mode == CORE_CAN_MODE_CLOSED)
    {
        snprintf(resp->data, sizeof(resp->data), "\r");
        return;
    }

    cs->mode = CORE_CAN_MODE_CLOSED;

#if CORE_SLCAN_STRESS_TEST_NO_CAN
    snprintf(resp->data, sizeof(resp->data), "\r");
    DebugUART_Print("[CORE] id=%u ch%u channel CLOSED (NO_CAN stress mode)\r\n", (unsigned)id, (unsigned)ch);
#else
    if (g_bus[ch].open_client_count > 0U) { g_bus[ch].open_client_count--; }

    if (g_bus[ch].open_client_count == 0U)
    {
        if (CanTask_Close(ch) != 0)
        {
            DebugUART_Print("[CORE] id=%u ch%u WARNING: CanTask_Close failed on last-closer\r\n",
                            (unsigned)id, (unsigned)ch);
        }
        g_bus[ch].mode = CORE_CAN_MODE_CLOSED;
        g_bus[ch].bitrate_bps = 0;
        DebugUART_Print("[CORE] id=%u ch%u channel CLOSED -- was last opener, bus physically stopped\r\n",
                        (unsigned)id, (unsigned)ch);
    }
    else
    {
        DebugUART_Print("[CORE] id=%u ch%u channel CLOSED -- bus stays open (open_count=%u)\r\n",
                        (unsigned)id, (unsigned)ch, (unsigned)g_bus[ch].open_client_count);
    }

    snprintf(resp->data, sizeof(resp->data), "\r");
#endif
}

static core_cmd_result_t CoreTask_HandleEthCommand(const eth_cmd_msg_t *cmd_msg)
{
    uint8_t id = cmd_msg->client_id;
    slcan_cmd_t parsed;
    eth_resp_msg_t resp;

    memset(&parsed, 0, sizeof(parsed));
    memset(&resp, 0, sizeof(resp));
    resp.client_id = id;

    if (id >= MAX_CLIENTS)
    {
        DebugUART_Print("[CORE] ERROR: invalid client_id=%u in command\r\n", (unsigned)id);
        return CORE_CMD_DONE;
    }

    resp.channel_id = g_client_can[id].channel_id;   /* для справки/диагностики в ответе */

    if (strcmp(cmd_msg->data, "N\r") == 0)
    {
        snprintf(resp.data, sizeof(resp.data), NETCAN_SERIAL_RESPONSE);
        CoreTask_SendResponse(&resp);
        return CORE_CMD_DONE;
    }

    if (strcmp(cmd_msg->data, "V\r") == 0)
    {
        snprintf(resp.data, sizeof(resp.data), NETCAN_VERSION_RESPONSE);
        CoreTask_SendResponse(&resp);
        return CORE_CMD_DONE;
    }

    if (Slcan_ParseCommand(cmd_msg->data, &parsed) != 0)
    {
        DebugUART_Print("[CORE] id=%u parse ERROR: unsupported or invalid command\r\n", (unsigned)id);
        snprintf(resp.data, sizeof(resp.data), "\a");
    }
    else
    {
        client_can_state_t *cs = &g_client_can[id];

        if (parsed.type == SLCAN_CMD_SET_BITRATE)
        {
            CoreTask_PrintBitrate("[CORE] parsed bitrate: ", parsed.bitrate_code, parsed.bitrate_bps);
        }

        switch (parsed.type)
        {
            case SLCAN_CMD_OPEN:
                (void)CoreTask_TryOpenChannel(id, CORE_CAN_MODE_NORMAL, &resp);
                break;

            case SLCAN_CMD_CLOSE:
                CoreTask_CloseChannel(id, &resp);
                break;

            case SLCAN_CMD_LISTEN:
                (void)CoreTask_TryOpenChannel(id, CORE_CAN_MODE_LISTEN_ONLY, &resp);
                break;

            case SLCAN_CMD_SELF_RECEPTION:
                (void)CoreTask_TryOpenChannel(id, CORE_CAN_MODE_SELF_RECEPTION, &resp);
                break;

            case SLCAN_CMD_SET_BITRATE:
                if (cs->mode != CORE_CAN_MODE_CLOSED)
                {
                    snprintf(resp.data, sizeof(resp.data), "\a");
                    DebugUART_Print("[CORE] id=%u ERROR: bitrate change while channel open\r\n", (unsigned)id);
                }
                else
                {
                    char prefix[32];
                    cs->bitrate_code = parsed.bitrate_code;
                    cs->bitrate_bps  = parsed.bitrate_bps;
                    snprintf(resp.data, sizeof(resp.data), "\r");

                    snprintf(prefix, sizeof(prefix), "[CORE] id=%u bitrate set: ", (unsigned)id);
                    CoreTask_PrintBitrate(prefix, cs->bitrate_code, cs->bitrate_bps);
                }
                break;

            case SLCAN_CMD_SEND_FRAME:
            {
                can_msg_t can_msg;
                memset(&can_msg, 0, sizeof(can_msg));
                can_msg.frame = parsed.frame;

                if (cs->mode == CORE_CAN_MODE_CLOSED)
                {
                    snprintf(resp.data, sizeof(resp.data), "\a");
                    DebugUART_Print("[CORE] id=%u ERROR: cannot send frame, channel is CLOSED\r\n", (unsigned)id);
                }
                else if (cs->mode == CORE_CAN_MODE_LISTEN_ONLY)
                {
                    snprintf(resp.data, sizeof(resp.data), "\a");
                    DebugUART_Print("[CORE] id=%u ERROR: cannot send frame in LISTEN ONLY mode\r\n", (unsigned)id);
                }
                else
                {
                    if (cs->mode == CORE_CAN_MODE_SELF_RECEPTION)
                    {
                        can_msg.frame.Flags |= CAN_FLAG_SELF_RX;
                    }

#if CORE_SLCAN_STRESS_TEST_NO_CAN
                    CoreTask_SetFrameAck(&can_msg.frame, &resp);
#else
                    /* Подтверждение "Z\r" должно гарантированно влезть в
                     * core_to_eth_queue: иначе кадр уйдёт на шину, а ПК так и
                     * не узнает, что он принят. CoreTask -- единственный
                     * писатель этой очереди, поэтому проверка места заранее
                     * надёжна. Нет места -- тоже back-pressure, не ошибка. */
                    if (osMessageQueueGetSpace(core_to_eth_queue) == 0U)
                    {
                        g_core_to_eth_backpressure++;
                        if ((g_core_to_eth_backpressure == 1U) || ((g_core_to_eth_backpressure % 10000U) == 0U))
                        {
                            DebugUART_Print("[CORE] back-pressure: core_to_eth_queue full (events=%lu)\r\n",
                                            (unsigned long)g_core_to_eth_backpressure);
                        }
                        return CORE_CMD_RETRY;
                    }

                    if (osMessageQueuePut(core_to_can_queues[cs->channel_id], &can_msg, 0, 0) != osOK)
                    {
                        /* Очередь в CAN полна -- это НЕ ошибка, а back-pressure:
                         * ничего не отвечаем и ничего не выбрасываем, команду
                         * держит у себя CoreTask и повторит позже. Пока она
                         * висит, CoreTask не берёт новые из eth_to_core_queue --
                         * так затор доходит до TCP-окна и тормозит сам ПК. */
                        uint32_t n = ++g_core_to_can_backpressure[cs->channel_id];
                        if ((n == 1U) || ((n % 10000U) == 0U))
                        {
                            DebugUART_Print("[CORE] ch%u back-pressure: core_to_can_queue full (events=%lu)\r\n",
                                            (unsigned)cs->channel_id, (unsigned long)n);
                        }
                        return CORE_CMD_RETRY;
                    }
                    CoreTask_SetFrameAck(&can_msg.frame, &resp);
#endif
                }
                break;
            }

            default:
                snprintf(resp.data, sizeof(resp.data), "\a");
                DebugUART_Print("[CORE] id=%u ERROR: unsupported parsed command type\r\n", (unsigned)id);
                break;
        }
    }

    CoreTask_SendResponse(&resp);
    return CORE_CMD_DONE;
}

/*
 * Кадр реально пришёл с шины канала channel_id -- рассылаем ТОЛЬКО
 * тем клиентам, кто привязан ИМЕННО к этому каналу И сам открыл его
 * (mode != CLOSED).
 */
static void CoreTask_HandleCanRx(uint8_t channel_id, const can_msg_t *can_msg)
{
    char formatted[ETH_RESP_MAX_LEN];

    if ((can_msg->frame.Flags & CAN_FLAG_RTR) != 0U)
    {
        DebugUART_Print("[CORE] ch%u CAN RX RTR: ID=0x%08lX DLC=%u FLAGS=0x%02X\r\n",
                        (unsigned)channel_id,
                        (unsigned long)can_msg->frame.Id,
                        (unsigned)can_msg->frame.Size,
                        (unsigned)can_msg->frame.Flags);
    }

    if (Slcan_FormatFrame(&can_msg->frame, formatted, sizeof(formatted)) != 0)
    {
        DebugUART_Print("[CORE] ch%u ERROR: Slcan_FormatFrame failed\r\n", (unsigned)channel_id);
        return;
    }

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        eth_resp_msg_t resp;

        if (g_client_can[id].channel_id != channel_id) { continue; }
        if (g_client_can[id].mode == CORE_CAN_MODE_CLOSED) { continue; }

        memset(&resp, 0, sizeof(resp));
        resp.client_id = id;
        resp.channel_id = channel_id;
        strncpy(resp.data, formatted, sizeof(resp.data) - 1U);

        CoreTask_SendResponse(&resp);
    }
}

/*
 * Вызывать из raw_tcp_server.c сразу после выделения клиенту слота --
 * channel_id определяется тем, на какой TCP-порт пришло подключение.
 */
void CoreTask_ClientConnected(uint8_t client_id, uint8_t channel_id)
{
    if (client_id >= MAX_CLIENTS) { return; }
    if (channel_id >= NUM_CHANNELS) { return; }

    g_client_can[client_id].channel_id   = channel_id;
    g_client_can[client_id].bitrate_code = 8U;
    g_client_can[client_id].bitrate_bps  = 1000000U;
    g_client_can[client_id].mode         = CORE_CAN_MODE_CLOSED;
}

/*
 * Вызывать из raw_tcp_server.c при потере клиента -- кладёт
 * синтетическую команду "C\r" от имени этого клиента, чтобы закрытие
 * (и, возможно, физическая остановка канала, если это был последний
 * открывший) прошло через штатный, однопоточный путь в CoreTask.
 */
void CoreTask_NotifyClientGone(uint8_t client_id)
{
    eth_cmd_msg_t msg;

    if (client_id >= MAX_CLIENTS) { return; }

    memset(&msg, 0, sizeof(msg));
    msg.client_id = client_id;
    msg.channel_id = g_client_can[client_id].channel_id;
    snprintf(msg.data, sizeof(msg.data), "C\r");

    (void)osMessageQueuePut(eth_to_core_queue, &msg, 0, 0);
}

static void CoreTask(void *argument)
{
    (void)argument;

    eth_cmd_msg_t cmd_msg;
    can_msg_t can_msg;
    static uint32_t heartbeat_processed = 0;

    /* Команда, которую не удалось обработать (нет места в core_to_can_queue
     * или в core_to_eth_queue). Пока она есть, новые команды из
     * eth_to_core_queue НЕ берём -- это и создаёт back-pressure вплоть до
     * TCP-окна. */
    static eth_cmd_msg_t pending_cmd;
    static uint8_t       pending_cmd_valid = 0;
    static uint32_t      pending_since_tick = 0;
    static uint32_t      pending_timeout_drops = 0;

    DebugUART_Print("[CORE] CoreTask started\r\n");
#if CORE_SLCAN_STRESS_TEST_NO_CAN
    DebugUART_Print("[CORE] MODE: SLCAN STRESS TEST WITHOUT CAN\r\n");
#else
    DebugUART_Print("[CORE] MODE: REAL CAN ENABLED, MAX_CLIENTS=%u, NUM_CHANNELS=%u\r\n",
                    (unsigned)MAX_CLIENTS, (unsigned)NUM_CHANNELS);
#endif

    DebugUART_Print("[CORE] eth_to_core_queue=%p core_to_eth_queue=%p\r\n",
                    (void*)eth_to_core_queue, (void*)core_to_eth_queue);

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++)
    {
        DebugUART_Print("[CORE] ch%u core_to_can_queue=%p can_to_core_queue=%p\r\n",
                        (unsigned)ch,
                        (void*)core_to_can_queues[ch],
                        (void*)can_to_core_queues[ch]);
    }

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        g_client_can[id].channel_id   = 0;
        g_client_can[id].bitrate_code = 8U;
        g_client_can[id].bitrate_bps  = 1000000U;
        g_client_can[id].mode         = CORE_CAN_MODE_CLOSED;
    }

    for (;;)
    {
        uint32_t processed = 0;
        uint8_t  eth_blocked = 0;

        /* 1. Сначала -- отложенная команда (порядок команд не нарушается). */
        if (pending_cmd_valid)
        {
            if (CoreTask_HandleEthCommand(&pending_cmd) == CORE_CMD_DONE)
            {
                pending_cmd_valid = 0;
                processed++;
                heartbeat_processed++;
            }
            else if ((osKernelGetTickCount() - pending_since_tick) > CORE_PENDING_MAX_MS)
            {
                /* Места нет слишком долго (шина без ACK, bus-off...) --
                 * это авария, а не перегрузка. Отбрасываем с BELL, чтобы не
                 * заблокировать навсегда всех клиентов (включая их "C\r"). */
                eth_resp_msg_t resp;
                memset(&resp, 0, sizeof(resp));
                resp.client_id  = pending_cmd.client_id;
                resp.channel_id = pending_cmd.channel_id;
                snprintf(resp.data, sizeof(resp.data), "\a");
                CoreTask_SendResponse(&resp);

                pending_cmd_valid = 0;
                pending_timeout_drops++;
                DebugUART_Print("[CORE] id=%u ch%u ERROR: frame dropped after %u ms waiting for queue space "
                                "(total timeout drops=%lu)\r\n",
                                (unsigned)pending_cmd.client_id, (unsigned)pending_cmd.channel_id,
                                (unsigned)CORE_PENDING_MAX_MS, (unsigned long)pending_timeout_drops);
                processed++;
            }
            else
            {
                eth_blocked = 1;
            }
        }

        /* 2. Новые команды -- только если ничего не висит. */
        if (!eth_blocked)
        {
            for (uint32_t i = 0; i < 128; i++)
            {
                if (osMessageQueueGet(eth_to_core_queue, &cmd_msg, NULL, 0) != osOK)
                {
                    break;
                }

                if (CoreTask_HandleEthCommand(&cmd_msg) == CORE_CMD_RETRY)
                {
                    pending_cmd        = cmd_msg;
                    pending_cmd_valid  = 1;
                    pending_since_tick = osKernelGetTickCount();
                    break;
                }

                processed++;
                heartbeat_processed++;
                if ((heartbeat_processed % 5000U) == 0U)
                {
                    DebugUART_Print("[CORE] heartbeat: processed=%lu\r\n", (unsigned long)heartbeat_processed);
                }
            }
        }

        /* 3. CAN RX обслуживается ВСЕГДА, даже когда ETH-вход заторможен.
         * Но кадр забираем из can_to_core_queue, только если в
         * core_to_eth_queue есть место под рассылку всем клиентам канала:
         * иначе он потерялся бы здесь, а так подождёт в can_to_core_queue
         * (её переполнение честно видно в счётчике queue_drop у CanTask). */
        for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++)
        {
            for (uint32_t i = 0; i < 128; i++)
            {
                if (osMessageQueueGetSpace(core_to_eth_queue) < (uint32_t)MAX_CLIENTS)
                {
                    break;
                }

                if (osMessageQueueGet(can_to_core_queues[ch], &can_msg, NULL, 0) == osOK)
                {
                    CoreTask_HandleCanRx(ch, &can_msg);
                    processed++;
                }
                else
                {
                    break;
                }
            }
        }

        if (processed == 0)
        {
            osDelay(1);
        }
    }
}

void CoreTask_Start(void)
{
    const osThreadAttr_t attr = {
        .name = "CoreTask",
        .stack_size = 8192,
        .priority = (osPriority_t)osPriorityNormal
    };

    coreTaskHandle = osThreadNew(CoreTask, NULL, &attr);

    if (!coreTaskHandle)
    {
        DebugUART_Print("[CORE] ERROR: task create failed\r\n");
    }
    else
    {
        DebugUART_Print("[CORE] task created\r\n");
    }
}

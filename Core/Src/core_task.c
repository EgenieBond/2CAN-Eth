/*
 * core_task.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *  Поддержка нескольких одновременных клиентов на один физический CAN-канал.
 *
 *  Состояние канала (открыт/закрыт, режим) -- ПЕРСОНАЛЬНОЕ у каждого
 *  клиента (g_client_can[]), как у реальных SLCAN-адаптеров.
 *
 *  Битрейт и физический режим FDCAN -- ОБЩИЙ ресурс на всю шину
 *  (g_bus): первый клиент, открывший канал, физически стартует FDCAN
 *  с выбранными им параметрами; последующие клиенты присоединяются,
 *  только если их запрошенные режим и битрейт совпадают с уже
 *  работающими -- иначе получают отказ. Шина физически останавливается,
 *  когда закрывается последний из открывших её клиентов.
 *
 *  Приём CAN-кадров рассылается только тем клиентам, кто сам открыл
 *  канал, не всем подряд.
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

/*
 * 1 = стресс-тест SLCAN без физического CAN:
 *     TCP -> Ethernet -> Core -> Parser -> z/Z -> TCP.
 *     Кадры НЕ кладутся в core_to_can_queue, физическая шина не
 *     арбитрируется (открыть может любой клиент в любой момент).
 *
 * 0 = обычный рабочий режим:
 *     TCP -> Ethernet -> Core -> CAN Task -> FDCAN.
 */
#define CORE_SLCAN_STRESS_TEST_NO_CAN 0

static osThreadId_t coreTaskHandle = NULL;

/* ===== Состояние канала на каждого клиента ===== */

typedef struct
{
    uint8_t         bitrate_code;
    uint32_t        bitrate_bps;
    core_can_mode_t mode;   		/* CLOSED, пока этот клиент сам не открыл канал */
} client_can_state_t;

static client_can_state_t g_client_can[MAX_CLIENTS];

/* ===== Общее состояние физической шины (один FDCAN на всех) ===== */

typedef struct
{
    uint8_t         open_client_count;  /* сколько клиентов сейчас держат шину открытой */
    core_can_mode_t mode;               /* режим, с которым шина реально запущена (валиден при count>0) */
    uint32_t        bitrate_bps;        /* битрейт, с которым шина реально запущена (валиден при count>0) */
} can_bus_state_t;

static can_bus_state_t g_bus =
{
    .open_client_count = 0,
    .mode               = CORE_CAN_MODE_CLOSED,
    .bitrate_bps        = 0
};

static void CoreTask_PrintBitrate(const char *prefix, uint8_t bitrate_code, uint32_t bitrate_bps)
{
    if (bitrate_code != 0xFFU)
    {
        DebugUART_Print("%sS%u -> %lu bit/s\r\n",
                        prefix,
                        (unsigned)bitrate_code,
                        (unsigned long)bitrate_bps);
    }
    else
    {
        DebugUART_Print("%sdirect %lu bit/s\r\n",
                        prefix,
                        (unsigned long)bitrate_bps);
    }
}

static void CoreTask_SendResponse(const eth_resp_msg_t *resp)
{
    if (osMessageQueuePut(core_to_eth_queue, resp, 0, 0) != osOK)
    {
        static uint32_t core_to_eth_full_count = 0;
        core_to_eth_full_count++;
        if ((core_to_eth_full_count % 200U) == 0U)
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
 * Возвращает 0 при успехе, -1 при отказе (пишет причину в resp как NAK).
 */
static int CoreTask_TryOpenChannel(uint8_t id, core_can_mode_t desired_mode, eth_resp_msg_t *resp)
{
    client_can_state_t *cs = &g_client_can[id];

    if (cs->mode != CORE_CAN_MODE_CLOSED)
    {
        snprintf(resp->data, sizeof(resp->data), "\a");
        DebugUART_Print("[CORE] id=%u ERROR: channel already open by this client\r\n",
                        (unsigned)id);
        return -1;
    }

#if CORE_SLCAN_STRESS_TEST_NO_CAN
    /* В NO_CAN-режиме реальной шины нет -- арбитраж не нужен, любой
     * клиент может "открыть" в любое время. */
    cs->mode = desired_mode;
    snprintf(resp->data, sizeof(resp->data), "\r");
    DebugUART_Print("[CORE] id=%u channel OPEN mode=%d (NO_CAN stress mode)\r\n",
                    (unsigned)id, (int)desired_mode);
    CoreTask_PrintBitrate("[CORE] active bitrate: ", cs->bitrate_code, cs->bitrate_bps);
    return 0;
#else
    if (g_bus.open_client_count == 0U)
    {
        /* Этот клиент первый -- физически стартуем FDCAN с ЕГО параметрами. */
        if (CanTask_Open(desired_mode, cs->bitrate_bps) != 0)
        {
            snprintf(resp->data, sizeof(resp->data), "\a");
            DebugUART_Print("[CORE] id=%u ERROR: failed to open CAN (first opener)\r\n",
                            (unsigned)id);
            return -1;
        }

        g_bus.mode               = desired_mode;
        g_bus.bitrate_bps        = cs->bitrate_bps;
        g_bus.open_client_count  = 1U;
        cs->mode                 = desired_mode;

        snprintf(resp->data, sizeof(resp->data), "\r");
        DebugUART_Print("[CORE] id=%u channel OPEN mode=%d -- physically started bus\r\n",
                        (unsigned)id, (int)desired_mode);
        CoreTask_PrintBitrate("[CORE] active bitrate: ", cs->bitrate_code, cs->bitrate_bps);
        return 0;
    }
    else
    {
        /* Шина уже работает с другим клиентом (или несколькими) --
         * присоединяемся, только если режим И битрейт совпадают. */
        if ((g_bus.mode != desired_mode) || (g_bus.bitrate_bps != cs->bitrate_bps))
        {
            snprintf(resp->data, sizeof(resp->data), "\a");
            DebugUART_Print("[CORE] id=%u ERROR: bus busy with mode=%d bitrate=%lu, "
                            "requested mode=%d bitrate=%lu -- conflict\r\n",
                            (unsigned)id,
                            (int)g_bus.mode, (unsigned long)g_bus.bitrate_bps,
                            (int)desired_mode, (unsigned long)cs->bitrate_bps);
            return -1;
        }

        g_bus.open_client_count++;
        cs->mode = desired_mode;

        snprintf(resp->data, sizeof(resp->data), "\r");
        DebugUART_Print("[CORE] id=%u channel OPEN mode=%d -- joined existing bus "
                        "(open_count=%u)\r\n",
                        (unsigned)id, (int)desired_mode,
                        (unsigned)g_bus.open_client_count);
        return 0;
    }
#endif
}

/*
 * Закрывает канал для клиента id, если он был открыт. Если это был
 * последний открывший клиент -- физически останавливает FDCAN.
 */
static void CoreTask_CloseChannel(uint8_t id, eth_resp_msg_t *resp)
{
    client_can_state_t *cs = &g_client_can[id];

    if (cs->mode == CORE_CAN_MODE_CLOSED)
    {
        /* уже закрыт этим клиентом -- считаем успехом, как и раньше */
        snprintf(resp->data, sizeof(resp->data), "\r");
        return;
    }

    cs->mode = CORE_CAN_MODE_CLOSED;

#if CORE_SLCAN_STRESS_TEST_NO_CAN
    snprintf(resp->data, sizeof(resp->data), "\r");
    DebugUART_Print("[CORE] id=%u channel CLOSED (NO_CAN stress mode)\r\n", (unsigned)id);
#else
    if (g_bus.open_client_count > 0U)
    {
        g_bus.open_client_count--;
    }

    if (g_bus.open_client_count == 0U)
    {
        if (CanTask_Close() != 0)
        {
            DebugUART_Print("[CORE] id=%u WARNING: CanTask_Close failed on last-closer\r\n",
                            (unsigned)id);
        }
        g_bus.mode        = CORE_CAN_MODE_CLOSED;
        g_bus.bitrate_bps  = 0;
        DebugUART_Print("[CORE] id=%u channel CLOSED -- was last opener, bus physically stopped\r\n",
                        (unsigned)id);
    }
    else
    {
        DebugUART_Print("[CORE] id=%u channel CLOSED -- bus stays open (open_count=%u)\r\n",
                        (unsigned)id, (unsigned)g_bus.open_client_count);
    }

    snprintf(resp->data, sizeof(resp->data), "\r");
#endif
}

static void CoreTask_HandleEthCommand(const eth_cmd_msg_t *cmd_msg)
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
        return;
    }

    /*
     * NetCAN compatibility commands.
     */
    if (strcmp(cmd_msg->data, "N\r") == 0)
    {
        snprintf(resp.data, sizeof(resp.data), NETCAN_SERIAL_RESPONSE);
        CoreTask_SendResponse(&resp);
        return;
    }

    if (strcmp(cmd_msg->data, "V\r") == 0)
    {
        snprintf(resp.data, sizeof(resp.data), NETCAN_VERSION_RESPONSE);
        CoreTask_SendResponse(&resp);
        return;
    }

    if (Slcan_ParseCommand(cmd_msg->data, &parsed) != 0)
    {
        DebugUART_Print("[CORE] id=%u parse ERROR: unsupported or invalid command\r\n",
                        (unsigned)id);
        snprintf(resp.data, sizeof(resp.data), "\a");
    }
    else
    {
        client_can_state_t *cs = &g_client_can[id];

        if (parsed.type == SLCAN_CMD_SET_BITRATE)
        {
            CoreTask_PrintBitrate("[CORE] parsed bitrate: ",
                                  parsed.bitrate_code,
                                  parsed.bitrate_bps);
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
                    DebugUART_Print("[CORE] id=%u ERROR: bitrate change while channel open\r\n",
                                    (unsigned)id);
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
                    DebugUART_Print("[CORE] id=%u ERROR: cannot send frame, channel is CLOSED\r\n",
                                    (unsigned)id);
                }
                else if (cs->mode == CORE_CAN_MODE_LISTEN_ONLY)
                {
                    snprintf(resp.data, sizeof(resp.data), "\a");
                    DebugUART_Print("[CORE] id=%u ERROR: cannot send frame in LISTEN ONLY mode\r\n",
                                    (unsigned)id);
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
                    if (osMessageQueuePut(core_to_can_queue, &can_msg, 0, 0) != osOK)
                    {
                        static uint32_t core_to_can_full_count = 0;
                        snprintf(resp.data, sizeof(resp.data), "\a");
                        core_to_can_full_count++;
                        if ((core_to_can_full_count % 200U) == 0U)
                        {
                            DebugUART_Print("[CORE] ERROR: core_to_can_queue full (count=%lu)\r\n",
                                            (unsigned long)core_to_can_full_count);
                        }
                    }
                    else
                    {
                        CoreTask_SetFrameAck(&can_msg.frame, &resp);
                    }
#endif
                }
                break;
            }

            default:
                snprintf(resp.data, sizeof(resp.data), "\a");
                DebugUART_Print("[CORE] id=%u ERROR: unsupported parsed command type\r\n",
                                (unsigned)id);
                break;
        }
    }

    CoreTask_SendResponse(&resp);
}

/*
 * Кадр реально пришёл с шины -- рассылаем ТОЛЬКО тем клиентам, кто сам
 * открыл канал (mode != CLOSED), не всем подряд.
 */
static void CoreTask_HandleCanRx(const can_msg_t *can_msg)
{
    char formatted[ETH_RESP_MAX_LEN];

    if ((can_msg->frame.Flags & CAN_FLAG_RTR) != 0U)
    {
        DebugUART_Print("[CORE] CAN RX RTR: ID=0x%08lX DLC=%u FLAGS=0x%02X\r\n",
                        (unsigned long)can_msg->frame.Id,
                        (unsigned)can_msg->frame.Size,
                        (unsigned)can_msg->frame.Flags);
    }

    if (Slcan_FormatFrame(&can_msg->frame, formatted, sizeof(formatted)) != 0)
    {
        DebugUART_Print("[CORE] ERROR: Slcan_FormatFrame failed\r\n");
        return;
    }

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        eth_resp_msg_t resp;

        if (g_client_can[id].mode == CORE_CAN_MODE_CLOSED)
        {
            continue;   /* этот клиент канал не открывал -- кадр ему не адресован */
        }

        memset(&resp, 0, sizeof(resp));
        resp.client_id = id;
        strncpy(resp.data, formatted, sizeof(resp.data) - 1U);

        CoreTask_SendResponse(&resp);
    }
}

/*
 * Вызывать из raw_tcp_server.c сразу после выделения клиенту слота
 * (в tcp_server_accept) -- сбрасывает CAN-состояние именно этого id
 * на значения по умолчанию (канал закрыт, битрейт 1 Мбит/с).
 */
void CoreTask_ClientConnected(uint8_t client_id)
{
    if (client_id >= MAX_CLIENTS)
    {
        return;
    }

    g_client_can[client_id].bitrate_code = 8U;
    g_client_can[client_id].bitrate_bps  = 1000000U;
    g_client_can[client_id].mode         = CORE_CAN_MODE_CLOSED;
}

/*
 * Вызывать из raw_tcp_server.c при потере клиента (обрыв, штатное
 * закрытие, idle timeout) -- ДО того, как client_handler сбросит свои
 * буферы. Кладёт синтетическую команду "закрыть канал" от имени этого
 * клиента в eth_to_core_queue, чтобы фактическое закрытие (включая,
 * возможно, физическую остановку шины, если это был последний
 * открывший клиент) прошло через штатный, однопоточный путь обработки
 * в CoreTask -- без риска гонки с CanTask из tcpip_thread.
 */
void CoreTask_NotifyClientGone(uint8_t client_id)
{
    eth_cmd_msg_t msg;

    if (client_id >= MAX_CLIENTS)
    {
        return;
    }

    memset(&msg, 0, sizeof(msg));
    msg.client_id = client_id;
    snprintf(msg.data, sizeof(msg.data), "C\r");

    /* best-effort -- если очередь переполнена, канал этого клиента
     * останется помеченным открытым до перезапуска платы в редком
     * худшем случае; не критично для корректности остальной системы. */
    (void)osMessageQueuePut(eth_to_core_queue, &msg, 0, 0);
}

static void CoreTask(void *argument)
{
    (void)argument;

    eth_cmd_msg_t cmd_msg;
    can_msg_t can_msg;
    static uint32_t heartbeat_processed = 0;

    DebugUART_Print("[CORE] CoreTask started\r\n");

#if CORE_SLCAN_STRESS_TEST_NO_CAN
    DebugUART_Print("[CORE] MODE: SLCAN STRESS TEST WITHOUT CAN\r\n");
#else
    DebugUART_Print("[CORE] MODE: REAL CAN ENABLED, MAX_CLIENTS=%u\r\n",
                    (unsigned)MAX_CLIENTS);
#endif

    DebugUART_Print("[CORE] eth_to_core_queue=%p core_to_eth_queue=%p\r\n",
                    (void*)eth_to_core_queue,
                    (void*)core_to_eth_queue);
    DebugUART_Print("[CORE] core_to_can_queue=%p can_to_core_queue=%p\r\n",
                    (void*)core_to_can_queue,
                    (void*)can_to_core_queue);

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        g_client_can[id].bitrate_code = 8U;
        g_client_can[id].bitrate_bps  = 1000000U;
        g_client_can[id].mode         = CORE_CAN_MODE_CLOSED;
    }

    for (;;)
    {
        uint32_t processed = 0;

        for (uint32_t i = 0; i < 128; i++)
        {
            if (osMessageQueueGet(eth_to_core_queue, &cmd_msg, NULL, 0) == osOK)
            {
                CoreTask_HandleEthCommand(&cmd_msg);
                processed++;

                heartbeat_processed++;
                if ((heartbeat_processed % 5000U) == 0U)
                {
                    DebugUART_Print("[CORE] heartbeat: processed=%lu\r\n",
                                    (unsigned long)heartbeat_processed);
                }
            }
            else
            {
                break;
            }
        }

        for (uint32_t i = 0; i < 128; i++)
        {
            if (osMessageQueueGet(can_to_core_queue, &can_msg, NULL, 0) == osOK)
            {
                CoreTask_HandleCanRx(&can_msg);
                processed++;
            }
            else
            {
                break;
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

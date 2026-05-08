/*
 * core_task.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *  Это ядро между Ethernet и CAN. Отвечает за:
 *  - получение строки из eth_to_core_queue
 *  - вызов парсера
 *  - управление состоянием CAN-канала
 *  - отправку кадра в CAN через core_to_can_queue
 *  - прием кадра из can_to_core_queue
 *  - форматирование ответа в SLCAN и отправку в core_to_eth_queue
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
 *     Кадры НЕ кладутся в core_to_can_queue.
 *
 * 0 = обычный рабочий режим:
 *     TCP -> Ethernet -> Core -> CAN Task -> FDCAN.
 */
#define CORE_SLCAN_STRESS_TEST_NO_CAN 0

static osThreadId_t coreTaskHandle = NULL;
static const char *CoreTask_CmdTypeToStr(slcan_cmd_type_t type);

typedef struct
{
    uint8_t bitrate_code;
    uint32_t bitrate_bps;
    core_can_mode_t mode;
} core_state_t;

static core_state_t g_core_state =
{
    .bitrate_code = 8,
    .bitrate_bps  = 1000000U,
    .mode         = CORE_CAN_MODE_CLOSED
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
        /*
         * При нагрузочном тесте постоянный UART-лог сам тормозит систему.
         * Поэтому здесь можно оставить только короткую ошибку.
         */
        DebugUART_Print("[CORE] ERROR: core_to_eth_queue full\r\n");
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

static void CoreTask_HandleEthCommand(const eth_cmd_msg_t *cmd_msg)
{
    slcan_cmd_t parsed;
    eth_resp_msg_t resp;

    memset(&parsed, 0, sizeof(parsed));
    memset(&resp, 0, sizeof(resp));

    /*
     * NetCAN compatibility commands.
     *
     * N\r -> serial number
     * V\r -> firmware/version string
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
        DebugUART_Print("[CORE] parse ERROR: unsupported or invalid command\r\n");
        snprintf(resp.data, sizeof(resp.data), "\a");
    }
    else
    {
        if (parsed.type == SLCAN_CMD_SET_BITRATE)
        {
            CoreTask_PrintBitrate("[CORE] parsed bitrate: ",
                                  parsed.bitrate_code,
                                  parsed.bitrate_bps);
        }

        switch (parsed.type)
        {
            case SLCAN_CMD_OPEN:
#if CORE_SLCAN_STRESS_TEST_NO_CAN
                g_core_state.mode = CORE_CAN_MODE_NORMAL;
                snprintf(resp.data, sizeof(resp.data), "\r");
                DebugUART_Print("[CORE] channel OPEN (NO_CAN stress mode)\r\n");
                CoreTask_PrintBitrate("[CORE] active bitrate: ",
                                      g_core_state.bitrate_code,
                                      g_core_state.bitrate_bps);
#else
                if (CanTask_Open(CORE_CAN_MODE_NORMAL, g_core_state.bitrate_bps) != 0)
                {
                    snprintf(resp.data, sizeof(resp.data), "\a");
                    DebugUART_Print("[CORE] ERROR: failed to open CAN in NORMAL mode\r\n");
                }
                else
                {
                    g_core_state.mode = CORE_CAN_MODE_NORMAL;
                    snprintf(resp.data, sizeof(resp.data), "\r");
                    DebugUART_Print("[CORE] channel OPEN\r\n");
                    CoreTask_PrintBitrate("[CORE] active bitrate: ",
                                          g_core_state.bitrate_code,
                                          g_core_state.bitrate_bps);
                }
#endif
                break;

            case SLCAN_CMD_CLOSE:
#if CORE_SLCAN_STRESS_TEST_NO_CAN
                g_core_state.mode = CORE_CAN_MODE_CLOSED;
                snprintf(resp.data, sizeof(resp.data), "\r");
                DebugUART_Print("[CORE] channel CLOSED (NO_CAN stress mode)\r\n");
#else
                if (CanTask_Close() != 0)
                {
                    snprintf(resp.data, sizeof(resp.data), "\a");
                    DebugUART_Print("[CORE] ERROR: failed to close CAN\r\n");
                }
                else
                {
                    g_core_state.mode = CORE_CAN_MODE_CLOSED;
                    snprintf(resp.data, sizeof(resp.data), "\r");
                    DebugUART_Print("[CORE] channel CLOSED\r\n");
                }
#endif
                break;

            case SLCAN_CMD_LISTEN:
#if CORE_SLCAN_STRESS_TEST_NO_CAN
                g_core_state.mode = CORE_CAN_MODE_LISTEN_ONLY;
                snprintf(resp.data, sizeof(resp.data), "\r");
                DebugUART_Print("[CORE] channel LISTEN ONLY (NO_CAN stress mode)\r\n");
                CoreTask_PrintBitrate("[CORE] active bitrate: ",
                                      g_core_state.bitrate_code,
                                      g_core_state.bitrate_bps);
#else
                if (CanTask_Open(CORE_CAN_MODE_LISTEN_ONLY, g_core_state.bitrate_bps) != 0)
                {
                    snprintf(resp.data, sizeof(resp.data), "\a");
                    DebugUART_Print("[CORE] ERROR: failed to open CAN in LISTEN ONLY mode\r\n");
                }
                else
                {
                    g_core_state.mode = CORE_CAN_MODE_LISTEN_ONLY;
                    snprintf(resp.data, sizeof(resp.data), "\r");
                    DebugUART_Print("[CORE] channel LISTEN ONLY\r\n");
                    CoreTask_PrintBitrate("[CORE] active bitrate: ",
                                          g_core_state.bitrate_code,
                                          g_core_state.bitrate_bps);
                }
#endif
                break;

            case SLCAN_CMD_SELF_RECEPTION:
#if CORE_SLCAN_STRESS_TEST_NO_CAN
                g_core_state.mode = CORE_CAN_MODE_SELF_RECEPTION;
                snprintf(resp.data, sizeof(resp.data), "\r");
                DebugUART_Print("[CORE] channel SELF RECEPTION (NO_CAN stress mode)\r\n");
                CoreTask_PrintBitrate("[CORE] active bitrate: ",
                                      g_core_state.bitrate_code,
                                      g_core_state.bitrate_bps);
#else
                if (CanTask_Open(CORE_CAN_MODE_SELF_RECEPTION, g_core_state.bitrate_bps) != 0)
                {
                    snprintf(resp.data, sizeof(resp.data), "\a");
                    DebugUART_Print("[CORE] ERROR: failed to open CAN in SELF RECEPTION mode\r\n");
                }
                else
                {
                    g_core_state.mode = CORE_CAN_MODE_SELF_RECEPTION;
                    snprintf(resp.data, sizeof(resp.data), "\r");
                    DebugUART_Print("[CORE] channel SELF RECEPTION\r\n");
                    CoreTask_PrintBitrate("[CORE] active bitrate: ",
                                          g_core_state.bitrate_code,
                                          g_core_state.bitrate_bps);
                }
#endif
                break;

            case SLCAN_CMD_SET_BITRATE:
                if (g_core_state.mode != CORE_CAN_MODE_CLOSED)
                {
                    snprintf(resp.data, sizeof(resp.data), "\a");
                    DebugUART_Print("[CORE] ERROR: bitrate change while channel open\r\n");
                }
                else
                {
                    g_core_state.bitrate_code = parsed.bitrate_code;
                    g_core_state.bitrate_bps  = parsed.bitrate_bps;
                    snprintf(resp.data, sizeof(resp.data), "\r");

                    CoreTask_PrintBitrate("[CORE] bitrate set: ",
                                          g_core_state.bitrate_code,
                                          g_core_state.bitrate_bps);
                }
                break;

            case SLCAN_CMD_SEND_FRAME:
            {
                can_msg_t can_msg;
                memset(&can_msg, 0, sizeof(can_msg));
                can_msg.frame = parsed.frame;

                if (g_core_state.mode == CORE_CAN_MODE_CLOSED)
                {
                    snprintf(resp.data, sizeof(resp.data), "\a");
                    DebugUART_Print("[CORE] ERROR: cannot send frame, channel is CLOSED\r\n");
                }
                else if (g_core_state.mode == CORE_CAN_MODE_LISTEN_ONLY)
                {
                    snprintf(resp.data, sizeof(resp.data), "\a");
                    DebugUART_Print("[CORE] ERROR: cannot send frame in LISTEN ONLY mode\r\n");
                }
                else
                {
                    if (g_core_state.mode == CORE_CAN_MODE_SELF_RECEPTION)
                    {
                        can_msg.frame.Flags |= CAN_FLAG_SELF_RX;
                    }

#if CORE_SLCAN_STRESS_TEST_NO_CAN
                    /*
                     * Домашний стресс-тест:
                     * парсер и состояние канала проверяются,
                     * но кадр не отправляется в FDCAN.
                     */
                    CoreTask_SetFrameAck(&can_msg.frame, &resp);
#else
                    if (osMessageQueuePut(core_to_can_queue, &can_msg, 0, 0) != osOK)
                    {
                        snprintf(resp.data, sizeof(resp.data), "\a");
                        DebugUART_Print("[CORE] ERROR: core_to_can_queue full\r\n");
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
                DebugUART_Print("[CORE] ERROR: unsupported parsed command type\r\n");
                break;
        }
    }

    CoreTask_SendResponse(&resp);
}

static const char *CoreTask_CmdTypeToStr(slcan_cmd_type_t type)
{
    switch (type)
    {
        case SLCAN_CMD_OPEN:           return "OPEN";
        case SLCAN_CMD_CLOSE:          return "CLOSE";
        case SLCAN_CMD_LISTEN:         return "LISTEN";
        case SLCAN_CMD_SELF_RECEPTION: return "SELF_RECEPTION";
        case SLCAN_CMD_SET_BITRATE:    return "SET_BITRATE";
        case SLCAN_CMD_SEND_FRAME:     return "SEND_FRAME";
        default:                       return "UNKNOWN";
    }
}

static void CoreTask_HandleCanRx(const can_msg_t *can_msg)
{
    eth_resp_msg_t resp;

    memset(&resp, 0, sizeof(resp));

    if ((can_msg->frame.Flags & CAN_FLAG_RTR) != 0U)
    {
        DebugUART_Print("[CORE] CAN RX RTR: ID=0x%08lX DLC=%u FLAGS=0x%02X\r\n",
                        (unsigned long)can_msg->frame.Id,
                        (unsigned)can_msg->frame.Size,
                        (unsigned)can_msg->frame.Flags);
    }

    if (Slcan_FormatFrame(&can_msg->frame, resp.data, sizeof(resp.data)) == 0)
    {
        CoreTask_SendResponse(&resp);
    }
    else
    {
        DebugUART_Print("[CORE] ERROR: Slcan_FormatFrame failed\r\n");
    }
}

static void CoreTask(void *argument)
{
    (void)argument;

    eth_cmd_msg_t cmd_msg;
    can_msg_t can_msg;

    DebugUART_Print("[CORE] CoreTask started\r\n");

#if CORE_SLCAN_STRESS_TEST_NO_CAN
    DebugUART_Print("[CORE] MODE: SLCAN STRESS TEST WITHOUT CAN\r\n");
#else
    DebugUART_Print("[CORE] MODE: REAL CAN ENABLED\r\n");
#endif

    DebugUART_Print("[CORE] eth_to_core_queue=%p core_to_eth_queue=%p\r\n",
                    (void*)eth_to_core_queue,
                    (void*)core_to_eth_queue);
    DebugUART_Print("[CORE] core_to_can_queue=%p can_to_core_queue=%p\r\n",
                    (void*)core_to_can_queue,
                    (void*)can_to_core_queue);

    for (;;)
    {
        uint32_t processed = 0;

        for (uint32_t i = 0; i < 128; i++)
        {
            if (osMessageQueueGet(eth_to_core_queue, &cmd_msg, NULL, 0) == osOK)
            {
                CoreTask_HandleEthCommand(&cmd_msg);
                processed++;
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

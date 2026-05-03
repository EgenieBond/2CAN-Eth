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

static osThreadId_t coreTaskHandle = NULL;
static const char *CoreTask_CmdTypeToStr(slcan_cmd_type_t type);

typedef struct
{
    uint8_t bitrate_code;      /* S0..S8 или 0xFF для прямого значения */
    uint32_t bitrate_bps;      /* реальный битрейт */
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

static void CoreTask_HandleEthCommand(const eth_cmd_msg_t *cmd_msg)
{
    slcan_cmd_t parsed;
    eth_resp_msg_t resp;

    memset(&parsed, 0, sizeof(parsed));
    memset(&resp, 0, sizeof(resp));

    //DebugUART_Print("[CORE] got cmd raw: ");
    for (size_t i = 0; i < strlen(cmd_msg->data); i++)
    {
        //DebugUART_Print("%02X ", (unsigned char)cmd_msg->data[i]);
    }
    //DebugUART_Print("\r\n");

    if (Slcan_ParseCommand(cmd_msg->data, &parsed) != 0)
    {
        DebugUART_Print("[CORE] parse ERROR: unsupported or invalid command\r\n");
        snprintf(resp.data, sizeof(resp.data), "\a");
    }
    else
    {
        //DebugUART_Print("[CORE] parse OK, type=%s\r\n", CoreTask_CmdTypeToStr(parsed.type));

        if (parsed.type == SLCAN_CMD_SET_BITRATE)
        {
            CoreTask_PrintBitrate("[CORE] parsed bitrate: ",
                                  parsed.bitrate_code,
                                  parsed.bitrate_bps);
        }

        if (parsed.type == SLCAN_CMD_SEND_FRAME)
        {
            if ((parsed.frame.Flags & CAN_FLAG_RTR) != 0U)
            {
                DebugUART_Print("[CORE] parsed RTR frame: ID=0x%08lX DLC=%u FLAGS=0x%02X\r\n",
                                (unsigned long)parsed.frame.Id,
                                (unsigned)parsed.frame.Size,
                                (unsigned)parsed.frame.Flags);
            }
            else
            {
            	/*
                DebugUART_Print("[CORE] parsed frame: ID=0x%08lX DLC=%u FLAGS=0x%02X DATA=",
                                (unsigned long)parsed.frame.Id,
                                (unsigned)parsed.frame.Size,
                                (unsigned)parsed.frame.Flags);


                for (uint8_t i = 0; i < parsed.frame.Size; i++)
                {
                    DebugUART_Print("%02X ", (unsigned)parsed.frame.Data[i]);
                }
                DebugUART_Print("\r\n");
                */
            }
        }

        switch (parsed.type)
        {
            case SLCAN_CMD_OPEN:
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
                break;

            case SLCAN_CMD_CLOSE:
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
                break;

            case SLCAN_CMD_LISTEN:
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
                break;

            case SLCAN_CMD_SELF_RECEPTION:
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

                    if (osMessageQueuePut(core_to_can_queue, &can_msg, 0, 0) != osOK)
                    {
                        snprintf(resp.data, sizeof(resp.data), "\a");
                        DebugUART_Print("[CORE] ERROR: core_to_can_queue full\r\n");
                    }
                    else
                    {
                        snprintf(resp.data, sizeof(resp.data), "\r");
                        /*
                        DebugUART_Print("[CORE] frame queued to CAN\r\n");
                        DebugUART_Print("[CORE] frame: ID=0x%08lX DLC=%u FLAGS=0x%02X\r\n",
                                        (unsigned long)can_msg.frame.Id,
                                        (unsigned)can_msg.frame.Size,
                                        (unsigned)can_msg.frame.Flags);
                        */
                    }
                }
                break;
            }

            default:
                snprintf(resp.data, sizeof(resp.data), "\a");
                DebugUART_Print("[CORE] ERROR: unsupported parsed command type\r\n");
                break;
        }
    }

    //DebugUART_Print("[CORE] resp str: %s\r\n", resp.data);
    /*
    DebugUART_Print("[CORE] resp raw: ");
    for (size_t i = 0; i < strlen(resp.data); i++)
    {
        DebugUART_Print("%02X ", (unsigned char)resp.data[i]);
    }
    DebugUART_Print("\r\n");
     */

    if (osMessageQueuePut(core_to_eth_queue, &resp, 0, 0) != osOK)
    {
        DebugUART_Print("[CORE] ERROR: core_to_eth_queue full\r\n");
    }
    else
    {
        //DebugUART_Print("[CORE] processed OK\r\n");
    }
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
    else
    {
    	/*
        DebugUART_Print("[CORE] CAN RX DATA: ID=0x%08lX DLC=%u FLAGS=0x%02X DATA=",
                        (unsigned long)can_msg->frame.Id,
                        (unsigned)can_msg->frame.Size,
                        (unsigned)can_msg->frame.Flags);

        for (uint8_t i = 0; i < can_msg->frame.Size; i++)
        {
            DebugUART_Print("%02X ", (unsigned)can_msg->frame.Data[i]);
        }
        DebugUART_Print("\r\n");
        */
    }

    if (Slcan_FormatFrame(&can_msg->frame, resp.data, sizeof(resp.data)) == 0)
    {
        //DebugUART_Print("[CORE] formatted SLCAN: %s\r\n", resp.data);

        if (osMessageQueuePut(core_to_eth_queue, &resp, 0, 0) != osOK)
        {
            DebugUART_Print("[CORE] ERROR: core_to_eth_queue full (CAN RX)\r\n");
        }
        else
        {
            //DebugUART_Print("[CORE] CAN RX -> ETH OK\r\n");
        }
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
    DebugUART_Print("[CORE] eth_to_core_queue=%p core_to_eth_queue=%p\r\n",
                    (void*)eth_to_core_queue,
                    (void*)core_to_eth_queue);
    DebugUART_Print("[CORE] core_to_can_queue=%p can_to_core_queue=%p\r\n",
                    (void*)core_to_can_queue,
                    (void*)can_to_core_queue);

    for (;;)
    {
        if (osMessageQueueGet(eth_to_core_queue, &cmd_msg, NULL, 10) == osOK)
        {
            CoreTask_HandleEthCommand(&cmd_msg);
        }

        if (osMessageQueueGet(can_to_core_queue, &can_msg, NULL, 0) == osOK)
        {
            CoreTask_HandleCanRx(&can_msg);
        }

        osDelay(1);
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

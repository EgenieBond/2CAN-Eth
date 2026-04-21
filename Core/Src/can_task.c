/*
 * can_task.c
 *
 *  Created on: Mar 10, 2026
 *      Author: Egenie
 */

#include "can_task.h"
#include "cmsis_os.h"
#include "app_queues.h"
#include "debug_uart.h"
#include "can_types.h"
#include "main.h"

#include <string.h>
#include <stdint.h>

static osThreadId_t canTaskHandle = NULL;
extern FDCAN_HandleTypeDef hfdcan1;

static const char *CanTask_IdTypeToStr(uint32_t id_type)
{
    return (id_type == FDCAN_EXTENDED_ID) ? "EXTENDED" : "STANDARD";
}

static const char *CanTask_FrameTypeToStr(uint32_t frame_type)
{
    return (frame_type == FDCAN_REMOTE_FRAME) ? "RTR" : "DATA";
}

static int CanTask_BuildTxHeader(const can_frame_t *frame, FDCAN_TxHeaderTypeDef *hdr)
{
    if ((frame == NULL) || (hdr == NULL))
    {
        return -1;
    }

    if (frame->Size > 8U)
    {
        return -1;
    }

    memset(hdr, 0, sizeof(*hdr));

    hdr->Identifier = frame->Id;

    if ((frame->Flags & CAN_FLAG_EXTENDED) != 0U)
    {
        hdr->IdType = FDCAN_EXTENDED_ID;
    }
    else
    {
        if (frame->Id > 0x7FFU)
        {
            return -1;
        }

        hdr->IdType = FDCAN_STANDARD_ID;
    }

    if ((frame->Flags & CAN_FLAG_RTR) != 0U)
    {
        hdr->TxFrameType = FDCAN_REMOTE_FRAME;
    }
    else
    {
        hdr->TxFrameType = FDCAN_DATA_FRAME;
    }

    switch (frame->Size)
    {
        case 0: hdr->DataLength = FDCAN_DLC_BYTES_0; break;
        case 1: hdr->DataLength = FDCAN_DLC_BYTES_1; break;
        case 2: hdr->DataLength = FDCAN_DLC_BYTES_2; break;
        case 3: hdr->DataLength = FDCAN_DLC_BYTES_3; break;
        case 4: hdr->DataLength = FDCAN_DLC_BYTES_4; break;
        case 5: hdr->DataLength = FDCAN_DLC_BYTES_5; break;
        case 6: hdr->DataLength = FDCAN_DLC_BYTES_6; break;
        case 7: hdr->DataLength = FDCAN_DLC_BYTES_7; break;
        case 8: hdr->DataLength = FDCAN_DLC_BYTES_8; break;
        default:
            return -1;
    }

    hdr->ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    hdr->BitRateSwitch = FDCAN_BRS_OFF;
    hdr->FDFormat = FDCAN_CLASSIC_CAN;
    hdr->TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    hdr->MessageMarker = 0;

    return 0;
}

static void CanTask_PrintInputFrame(const can_frame_t *frame)
{
    if (frame == NULL)
    {
        return;
    }

    if ((frame->Flags & CAN_FLAG_RTR) != 0U)
    {
        DebugUART_Print("[CAN] frame from CORE: RTR ID=0x%08lX DLC=%u FLAGS=0x%02X\r\n",
                        (unsigned long)frame->Id,
                        (unsigned)frame->Size,
                        (unsigned)frame->Flags);
    }
    else
    {
        DebugUART_Print("[CAN] frame from CORE: DATA ID=0x%08lX DLC=%u FLAGS=0x%02X DATA=",
                        (unsigned long)frame->Id,
                        (unsigned)frame->Size,
                        (unsigned)frame->Flags);

        for (uint8_t i = 0; i < frame->Size; i++)
        {
            DebugUART_Print("%02X ", (unsigned)frame->Data[i]);
        }
        DebugUART_Print("\r\n");
    }
}

static void CanTask_PrintTxHeader(const FDCAN_TxHeaderTypeDef *hdr)
{
    if (hdr == NULL)
    {
        return;
    }

    DebugUART_Print("[CAN] TX header prepared:\r\n");
    DebugUART_Print("[CAN]   IdType     = %s\r\n", CanTask_IdTypeToStr(hdr->IdType));
    DebugUART_Print("[CAN]   Identifier = 0x%08lX\r\n", (unsigned long)hdr->Identifier);
    DebugUART_Print("[CAN]   FrameType  = %s\r\n", CanTask_FrameTypeToStr(hdr->TxFrameType));
    DebugUART_Print("[CAN]   DataLength = 0x%08lX\r\n", (unsigned long)hdr->DataLength);
}

static void CanTask_PrintPayload(const can_frame_t *frame)
{
    if (frame == NULL)
    {
        return;
    }

    if ((frame->Flags & CAN_FLAG_RTR) != 0U)
    {
        DebugUART_Print("[CAN] RTR frame: no payload bytes\r\n");
        return;
    }

    DebugUART_Print("[CAN] payload prepared: ");
    for (uint8_t i = 0; i < frame->Size; i++)
    {
        DebugUART_Print("%02X ", (unsigned)frame->Data[i]);
    }
    DebugUART_Print("\r\n");
}

static void CanTask(void *argument)
{
    (void)argument;

    can_msg_t can_msg;
    FDCAN_TxHeaderTypeDef tx_hdr;
    uint8_t tx_data[8];

    DebugUART_Print("[CAN] CanTask started\r\n");
    DebugUART_Print("[CAN] core_to_can_queue=%p can_to_core_queue=%p\r\n",
                    (void*)core_to_can_queue,
                    (void*)can_to_core_queue);

    for (;;)
    {
        if (osMessageQueueGet(core_to_can_queue, &can_msg, NULL, osWaitForever) == osOK)
        {
            memset(&tx_hdr, 0, sizeof(tx_hdr));
            memset(tx_data, 0, sizeof(tx_data));

            CanTask_PrintInputFrame(&can_msg.frame);

            if (CanTask_BuildTxHeader(&can_msg.frame, &tx_hdr) != 0)
            {
                DebugUART_Print("[CAN] ERROR: failed to build FDCAN TX header\r\n");
                continue;
            }

            memcpy(tx_data, can_msg.frame.Data, sizeof(tx_data));

            CanTask_PrintTxHeader(&tx_hdr);
            CanTask_PrintPayload(&can_msg.frame);

            if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &tx_hdr, tx_data) != HAL_OK)
            {
                uint32_t err = HAL_FDCAN_GetError(&hfdcan1);
                DebugUART_Print("[CAN] ERROR: HAL_FDCAN_AddMessageToTxFifoQ failed, err=0x%08lX\r\n",
                                (unsigned long)err);
                continue;
            }

            DebugUART_Print("[CAN] TX queued into FDCAN FIFO OK\r\n");
        }
    }
}

void CanTask_Start(void)
{
    const osThreadAttr_t attr = {
        .name = "CanTask",
        .stack_size = 4096,
        .priority = (osPriority_t)osPriorityNormal
    };

    canTaskHandle = osThreadNew(CanTask, NULL, &attr);

    if (!canTaskHandle)
    {
        DebugUART_Print("[CAN] ERROR: task create failed\r\n");
    }
    else
    {
        DebugUART_Print("[CAN] task created\r\n");
    }
}

static int CanTask_FdcanRxToCanFrame(const FDCAN_RxHeaderTypeDef *rx_hdr,
                                     const uint8_t *rx_data,
                                     can_frame_t *out_frame)
{
    if ((rx_hdr == NULL) || (out_frame == NULL))
    {
        return -1;
    }

    memset(out_frame, 0, sizeof(*out_frame));

    out_frame->Id = rx_hdr->Identifier;
    out_frame->Timestamp = 0U;

    if (rx_hdr->IdType == FDCAN_EXTENDED_ID)
    {
        out_frame->Flags |= CAN_FLAG_EXTENDED;
    }

    if (rx_hdr->RxFrameType == FDCAN_REMOTE_FRAME)
    {
        out_frame->Flags |= CAN_FLAG_RTR;
    }

    switch (rx_hdr->DataLength)
    {
        case FDCAN_DLC_BYTES_0: out_frame->Size = 0; break;
        case FDCAN_DLC_BYTES_1: out_frame->Size = 1; break;
        case FDCAN_DLC_BYTES_2: out_frame->Size = 2; break;
        case FDCAN_DLC_BYTES_3: out_frame->Size = 3; break;
        case FDCAN_DLC_BYTES_4: out_frame->Size = 4; break;
        case FDCAN_DLC_BYTES_5: out_frame->Size = 5; break;
        case FDCAN_DLC_BYTES_6: out_frame->Size = 6; break;
        case FDCAN_DLC_BYTES_7: out_frame->Size = 7; break;
        case FDCAN_DLC_BYTES_8: out_frame->Size = 8; break;
        default:
            return -1;
    }

    if (((out_frame->Flags & CAN_FLAG_RTR) == 0U) && (rx_data != NULL))
    {
        memcpy(out_frame->Data, rx_data, out_frame->Size);
    }

    return 0;
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    FDCAN_RxHeaderTypeDef rx_hdr;
    uint8_t rx_data[8];
    can_msg_t can_msg;

    if (hfdcan->Instance != FDCAN1)
    {
        return;
    }

    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) == 0U)
    {
        return;
    }

    memset(&rx_hdr, 0, sizeof(rx_hdr));
    memset(rx_data, 0, sizeof(rx_data));
    memset(&can_msg, 0, sizeof(can_msg));

    if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &rx_hdr, rx_data) != HAL_OK)
    {
        return;
    }

    if (CanTask_FdcanRxToCanFrame(&rx_hdr, rx_data, &can_msg.frame) != 0)
    {
        return;
    }

    if (osMessageQueuePut(can_to_core_queue, &can_msg, 0, 0) != osOK)
    {
        /* очередь переполнена */
        return;
    }
}

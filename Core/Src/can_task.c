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
#include <string.h>
#include <stdint.h>

static osThreadId_t canTaskHandle = NULL;

/* Значения сделаны в стиле будущего FDCAN,
   чтобы потом было легко перейти на HAL */
#define CAN_TX_ID_STANDARD   0U
#define CAN_TX_ID_EXTENDED   1U

#define CAN_TX_FRAME_DATA    0U
#define CAN_TX_FRAME_RTR     1U

/* Временная программная имитация входящего CAN-кадра */
#define CAN_DEBUG_LOOPBACK_TO_CORE  1

typedef struct
{
    uint32_t Identifier;
    uint32_t IdType;
    uint32_t TxFrameType;
    uint32_t DataLength;
} can_tx_debug_header_t;

static const char *CanTask_IdTypeToStr(uint32_t id_type)
{
    return (id_type == CAN_TX_ID_EXTENDED) ? "EXTENDED" : "STANDARD";
}

static const char *CanTask_FrameTypeToStr(uint32_t frame_type)
{
    return (frame_type == CAN_TX_FRAME_RTR) ? "RTR" : "DATA";
}

static int CanTask_BuildTxHeader(const can_frame_t *frame, can_tx_debug_header_t *hdr)
{
    if (!frame || !hdr)
        return -1;

    if (frame->Size > 8U)
        return -1;

    memset(hdr, 0, sizeof(*hdr));

    hdr->Identifier = frame->Id;
    hdr->DataLength = frame->Size;

    if (frame->Flags & CAN_FLAG_EXTENDED)
    {
        hdr->IdType = CAN_TX_ID_EXTENDED;
    }
    else
    {
        if (frame->Id > 0x7FFU)
            return -1;

        hdr->IdType = CAN_TX_ID_STANDARD;
    }

    if (frame->Flags & CAN_FLAG_RTR)
    {
        hdr->TxFrameType = CAN_TX_FRAME_RTR;
    }
    else
    {
        hdr->TxFrameType = CAN_TX_FRAME_DATA;
    }

    return 0;
}

static void CanTask_PrintInputFrame(const can_frame_t *frame)
{
    if (!frame)
        return;

    if (frame->Flags & CAN_FLAG_RTR)
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

static void CanTask_PrintTxHeader(const can_tx_debug_header_t *hdr)
{
    if (!hdr)
        return;

    DebugUART_Print("[CAN] TX header prepared:\r\n");
    DebugUART_Print("[CAN]   IdType     = %s\r\n", CanTask_IdTypeToStr(hdr->IdType));
    DebugUART_Print("[CAN]   Identifier = 0x%08lX\r\n", (unsigned long)hdr->Identifier);
    DebugUART_Print("[CAN]   FrameType  = %s\r\n", CanTask_FrameTypeToStr(hdr->TxFrameType));
    DebugUART_Print("[CAN]   DataLength = %lu\r\n", (unsigned long)hdr->DataLength);
}

static void CanTask_DebugLoopbackToCore(const can_frame_t *frame)
{
#if CAN_DEBUG_LOOPBACK_TO_CORE
    can_msg_t rx_msg;

    if (!frame)
        return;

    memset(&rx_msg, 0, sizeof(rx_msg));
    rx_msg.frame = *frame;

    if (osMessageQueuePut(can_to_core_queue, &rx_msg, 0, 0) != osOK)
    {
        DebugUART_Print("[CAN] ERROR: can_to_core_queue full (debug loopback)\r\n");
    }
    else
    {
        DebugUART_Print("[CAN] DEBUG: frame looped back to can_to_core_queue\r\n");
    }
#else
    (void)frame;
#endif
}

static void CanTask(void *argument)
{
    (void)argument;

    can_msg_t can_msg;
    can_tx_debug_header_t tx_hdr;

    DebugUART_Print("[CAN] CanTask started\r\n");
    DebugUART_Print("[CAN] core_to_can_queue=%p can_to_core_queue=%p\r\n",
                    (void*)core_to_can_queue,
                    (void*)can_to_core_queue);

    for (;;)
    {
        if (osMessageQueueGet(core_to_can_queue, &can_msg, NULL, osWaitForever) == osOK)
        {
            CanTask_PrintInputFrame(&can_msg.frame);

            if (CanTask_BuildTxHeader(&can_msg.frame, &tx_hdr) != 0)
            {
                DebugUART_Print("[CAN] ERROR: failed to build TX header\r\n");
                continue;
            }

            CanTask_PrintTxHeader(&tx_hdr);

            if ((can_msg.frame.Flags & CAN_FLAG_RTR) == 0U)
            {
                DebugUART_Print("[CAN] payload prepared: ");
                for (uint8_t i = 0; i < can_msg.frame.Size; i++)
                {
                    DebugUART_Print("%02X ", (unsigned)can_msg.frame.Data[i]);
                }
                DebugUART_Print("\r\n");
            }
            else
            {
                DebugUART_Print("[CAN] RTR frame: no payload bytes\r\n");
            }

            DebugUART_Print("[CAN] TEMP: frame is ready for future HAL_FDCAN_AddMessageToTxFifoQ()\r\n");

            CanTask_DebugLoopbackToCore(&can_msg.frame);
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

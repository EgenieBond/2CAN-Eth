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

static osThreadId_t canTaskHandle = NULL;

static void CanTask_PrintFrame(const can_frame_t *frame)
{
    if (!frame)
        return;

    if (frame->Flags & CAN_FLAG_RTR)
    {
        DebugUART_Print("[CAN] RX from CORE: RTR frame ID=0x%08lX DLC=%u FLAGS=0x%02X\r\n",
                        (unsigned long)frame->Id,
                        (unsigned)frame->Size,
                        (unsigned)frame->Flags);
    }
    else
    {
        DebugUART_Print("[CAN] RX from CORE: DATA frame ID=0x%08lX DLC=%u FLAGS=0x%02X DATA=",
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

static void CanTask(void *argument)
{
    (void)argument;
    can_msg_t can_msg;

    DebugUART_Print("[CAN] CanTask started\r\n");
    DebugUART_Print("[CAN] core_to_can_queue=%p can_to_core_queue=%p\r\n",
                    (void*)core_to_can_queue,
                    (void*)can_to_core_queue);

    for (;;)
    {
        if (osMessageQueueGet(core_to_can_queue, &can_msg, NULL, osWaitForever) == osOK)
        {
            CanTask_PrintFrame(&can_msg.frame);

            /* Пока это заглушка.
               Позже здесь будет реальная отправка через HAL_FDCAN_AddMessageToTxFifoQ(). */
            DebugUART_Print("[CAN] TEMP: frame consumed by debug CanTask\r\n");
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

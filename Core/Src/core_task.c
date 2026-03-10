/*
 * core_task.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *   Это ядро между Ethernet и CAN. Отвечает за:
 *  - получение строки из eth_to_core_queue
 *  - вызов парсера
 *  - отправку ответа в core_to_eth_queue
 */

#include "core_task.h"
#include "cmsis_os.h"
#include "app_queues.h"
#include "slcan_parser.h"
#include "debug_uart.h"
#include <string.h>

static osThreadId_t coreTaskHandle = NULL;

static void CoreTask(void *argument)
{
    (void)argument;
    eth_cmd_msg_t  cmd;
    eth_resp_msg_t resp;

    DebugUART_Print("[CORE] CoreTask started\r\n");

    for (;;)
    {
        if (osMessageQueueGet(eth_to_core_queue, &cmd, NULL, osWaitForever) == osOK)
        {
            memset(&resp, 0, sizeof(resp));

            Slcan_ProcessCommand(cmd.data, resp.data, sizeof(resp.data));

            if (osMessageQueuePut(core_to_eth_queue, &resp, 0, 0) != osOK)
            {
                DebugUART_Print("[CORE] ERROR: core_to_eth_queue full\r\n");
            }
            else
            {
                DebugUART_Print("[CORE] processed: %s", cmd.data);
            }
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
        DebugUART_Print("[CORE] ERROR: task create failed\r\n");
    else
        DebugUART_Print("[CORE] task created\r\n");
}

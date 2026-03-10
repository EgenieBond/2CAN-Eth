/*
 * eth_app.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *      Tasks:
 *      - промежуточный RX буфер клиента
 *      - сборка команды по \r
 *      - отправка готовой команды в eth_to_core_queue
 *      - чтение ответа из core_to_eth_queue
 *      - отправка ответа клиенту
 */

#include "eth_app.h"
#include "app_queues.h"
#include "client_handler.h"
#include "debug_uart.h"

osMessageQueueId_t eth_to_core_queue = NULL;
osMessageQueueId_t core_to_eth_queue = NULL;

void AppQueues_Init(void)
{
    eth_to_core_queue = osMessageQueueNew(8, sizeof(eth_cmd_msg_t), NULL);
    core_to_eth_queue = osMessageQueueNew(8, sizeof(eth_resp_msg_t), NULL);

    DebugUART_Print("[APP] eth_to_core_queue=%p item=%lu\r\n",
                    (void*)eth_to_core_queue,
                    (unsigned long)sizeof(eth_cmd_msg_t));

    DebugUART_Print("[APP] core_to_eth_queue=%p item=%lu\r\n",
                    (void*)core_to_eth_queue,
                    (unsigned long)sizeof(eth_resp_msg_t));

    if (!eth_to_core_queue || !core_to_eth_queue)
    {
        DebugUART_Print("[APP] ERROR: queue creation failed\r\n");
        while (1) { }
    }

    DebugUART_Print("[APP] Queues created OK\r\n");
}

void EthApp_Init(void)
{
    AppQueues_Init();
    ClientHandlerTask_Start();
    DebugUART_Print("[APP] EthApp_Init done\r\n");
}

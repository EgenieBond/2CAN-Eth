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
 *
 *  Обновлено: core_to_can_queue/can_to_core_queue -- теперь массивы,
 *  по паре на каждый физический CAN-канал (NUM_CHANNELS из app_queues.h).
 */

#include "eth_app.h"
#include "app_queues.h"
#include "client_handler.h"
#include "debug_uart.h"

#define ETH_TO_CORE_QUEUE_LEN   128
#define CORE_TO_ETH_QUEUE_LEN   256
#define CORE_TO_CAN_QUEUE_LEN   64
#define CAN_TO_CORE_QUEUE_LEN   64

osMessageQueueId_t eth_to_core_queue = NULL;
osMessageQueueId_t core_to_eth_queue = NULL;
osMessageQueueId_t core_to_can_queues[NUM_CHANNELS] = { NULL };
osMessageQueueId_t can_to_core_queues[NUM_CHANNELS] = { NULL };

void AppQueues_Init(void)
{
    eth_to_core_queue = osMessageQueueNew(ETH_TO_CORE_QUEUE_LEN,
                                          sizeof(eth_cmd_msg_t),
                                          NULL);

    core_to_eth_queue = osMessageQueueNew(CORE_TO_ETH_QUEUE_LEN,
                                          sizeof(eth_resp_msg_t),
                                          NULL);

    DebugUART_Print("[APP] eth_to_core_queue=%p len=%u item=%lu\r\n",
                    (void*)eth_to_core_queue,
                    (unsigned)ETH_TO_CORE_QUEUE_LEN,
                    (unsigned long)sizeof(eth_cmd_msg_t));

    DebugUART_Print("[APP] core_to_eth_queue=%p len=%u item=%lu\r\n",
                    (void*)core_to_eth_queue,
                    (unsigned)CORE_TO_ETH_QUEUE_LEN,
                    (unsigned long)sizeof(eth_resp_msg_t));

    uint8_t all_ok = (eth_to_core_queue != NULL) && (core_to_eth_queue != NULL);

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++)
    {
        core_to_can_queues[ch] = osMessageQueueNew(CORE_TO_CAN_QUEUE_LEN,
                                                   sizeof(can_msg_t),
                                                   NULL);

        can_to_core_queues[ch] = osMessageQueueNew(CAN_TO_CORE_QUEUE_LEN,
                                                   sizeof(can_msg_t),
                                                   NULL);

        DebugUART_Print("[APP] ch%u core_to_can_queue=%p len=%u item=%lu\r\n",
                        (unsigned)ch,
                        (void*)core_to_can_queues[ch],
                        (unsigned)CORE_TO_CAN_QUEUE_LEN,
                        (unsigned long)sizeof(can_msg_t));

        DebugUART_Print("[APP] ch%u can_to_core_queue=%p len=%u item=%lu\r\n",
                        (unsigned)ch,
                        (void*)can_to_core_queues[ch],
                        (unsigned)CAN_TO_CORE_QUEUE_LEN,
                        (unsigned long)sizeof(can_msg_t));

        all_ok = all_ok && (core_to_can_queues[ch] != NULL) && (can_to_core_queues[ch] != NULL);
    }

    if (!all_ok)
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

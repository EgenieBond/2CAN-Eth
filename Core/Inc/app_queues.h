/*
 * app_queues.h
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *  Поддержка ДВУХ независимых физических CAN-каналов (FDCAN1/FDCAN2).
 *  Каждый канал -- свой TCP-порт, свои CAN-очереди. Клиенты -- общий
 *  "плоский" пул на всю систему (MAX_CLIENTS=8), у каждого клиента
 *  метка channel_id -- к какому каналу он подключился (по порту).
 *
 *  eth_to_core_queue / core_to_eth_queue -- ОБЩИЕ на всю систему,
 *  CoreTask разбирает команды в одном потоке и смотрит на channel_id
 *  каждой команды, чтобы направить её в нужную пару CAN-очередей.
 *
 *  core_to_can_queues[] / can_to_core_queues[] -- РАЗДЕЛЬНЫЕ на каждый
 *  канал: реальная передача по FDCAN1 и FDCAN2 физически независима.
 */

#ifndef APP_QUEUES_H
#define APP_QUEUES_H

#include "cmsis_os.h"
#include <stdint.h>
#include "can_types.h"

#define ETH_CMD_MAX_LEN    64
#define ETH_RESP_MAX_LEN   64

#define NUM_CHANNELS       2U
#define CHANNEL_ID_INVALID 0xFFU

#define MAX_CLIENTS        8U

typedef struct
{
    uint8_t client_id;
    uint8_t channel_id;
    char    data[ETH_CMD_MAX_LEN];
} eth_cmd_msg_t;

typedef struct
{
    uint8_t client_id;
    uint8_t channel_id;
    char    data[ETH_RESP_MAX_LEN];
} eth_resp_msg_t;

typedef struct
{
    can_frame_t frame;
} can_msg_t;

extern osMessageQueueId_t eth_to_core_queue;
extern osMessageQueueId_t core_to_eth_queue;

extern osMessageQueueId_t core_to_can_queues[NUM_CHANNELS];
extern osMessageQueueId_t can_to_core_queues[NUM_CHANNELS];

void AppQueues_Init(void);

#endif /* APP_QUEUES_H */

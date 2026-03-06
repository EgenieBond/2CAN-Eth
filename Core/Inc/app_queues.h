/*
 * app_queues.h
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 */

#ifndef APP_QUEUES_H
#define APP_QUEUES_H

#include "cmsis_os.h"
#include <stdint.h>

#define ETH_CMD_MAX_LEN   64
#define ETH_RESP_MAX_LEN  64

typedef struct
{
    char data[ETH_CMD_MAX_LEN];
} eth_cmd_msg_t;

typedef struct
{
    char data[ETH_RESP_MAX_LEN];
} eth_resp_msg_t;

extern osMessageQueueId_t eth_to_core_queue;
extern osMessageQueueId_t core_to_eth_queue;

void AppQueues_Init(void);

#endif /* INC_APP_QUEUES_H_ */

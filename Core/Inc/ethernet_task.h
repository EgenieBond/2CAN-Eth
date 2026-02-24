/*
 * ethernet_task.h
 *
 *  Created on: Feb 4, 2026
 *      Author: Egenie
 */

#ifndef ETHERNET_TASK_H
#define ETHERNET_TASK_H

#include "cmsis_os.h"

extern osThreadId_t ethernetTaskHandle;
extern osMessageQueueId_t eth_to_core_queue;

#pragma once
#ifdef __cplusplus
}
#endif

void EthernetTask(void *argument);
void EthernetTask_Start(void);

#endif

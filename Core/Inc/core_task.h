/*
 * core_task.h
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *  Ядро между Ethernet и CAN. Обновлено под ДВА независимых физических
 *  канала -- CoreTask_ClientConnected теперь принимает channel_id
 *  (к какому каналу привязан клиент, по порту, на который он пришёл).
 */

#ifndef INC_CORE_TASK_H_
#define INC_CORE_TASK_H_

#include <stdint.h>

void CoreTask_Start(void);

void CoreTask_ClientConnected(uint8_t client_id, uint8_t channel_id);
void CoreTask_NotifyClientGone(uint8_t client_id);

#endif /* INC_CORE_TASK_H_ */

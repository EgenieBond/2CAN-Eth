/*
 * core_task.h
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *  Это ядро между Ethernet и CAN. Отвечает за:
 *  - получение строки из eth_to_core_queue
 *  - вызов парсера
 *  - отправку ответа в core_to_eth_queue
 *
 *  Обновлено: поддержка нескольких клиентов -- CoreTask_ClientConnected
 *  и CoreTask_NotifyClientGone вызываются из raw_tcp_server.c при
 *  подключении/потере клиента (см. подробные комментарии в core_task.c).
 */

#ifndef INC_CORE_TASK_H_
#define INC_CORE_TASK_H_

#include <stdint.h>

void CoreTask_Start(void);

void CoreTask_ClientConnected(uint8_t client_id);
void CoreTask_NotifyClientGone(uint8_t client_id);

#endif /* INC_CORE_TASK_H_ */

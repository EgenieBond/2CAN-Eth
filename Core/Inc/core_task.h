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
 */

#ifndef INC_CORE_TASK_H_
#define INC_CORE_TASK_H_

void CoreTask_Start(void);

#endif /* INC_CORE_TASK_H_ */

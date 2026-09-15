/*
 * can_task.h
 *
 *  Created on: Mar 10, 2026
 *      Author: Egenie
 *
 *  Обновлено: поддержка ДВУХ независимых физических CAN-каналов
 *  (FDCAN1 и FDCAN2). Каждый канал -- своя задача, свои очереди,
 *  свой хендл FDCAN. channel_id: 0 = FDCAN1, 1 = FDCAN2.
 */

#ifndef CAN_TASK_H
#define CAN_TASK_H

#include <stdint.h>

typedef enum
{
    CORE_CAN_MODE_CLOSED = 0,
    CORE_CAN_MODE_NORMAL,
    CORE_CAN_MODE_LISTEN_ONLY,
    CORE_CAN_MODE_SELF_RECEPTION
} core_can_mode_t;

/* Запускает задачу для указанного канала (0 или 1). Вызывать один раз
 * на каждый канал при старте платы. */
void CanTask_Start(uint8_t channel_id);

int CanTask_Open(uint8_t channel_id, core_can_mode_t mode, uint32_t bitrate_bps);
int CanTask_Close(uint8_t channel_id);

#endif /* CAN_TASK_H */

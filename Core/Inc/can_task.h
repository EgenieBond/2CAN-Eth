/*
 * can_task.h
 *
 *  Created on: Mar 10, 2026
 *      Author: Egenie
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

void CanTask_Start(void);
int CanTask_Open(core_can_mode_t mode, uint32_t bitrate_bps);
int CanTask_Close(void);

#endif /* CAN_TASK_H */

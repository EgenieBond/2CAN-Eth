/*
 * app_queues.h
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *  Обновлено: поддержка нескольких одновременных TCP-клиентов на один
 *  CAN-канал. Каждая команда/ответ теперь несёт client_id -- индекс
 *  клиента (0..MAX_CLIENTS-1), которому принадлежит команда или
 *  которому нужно доставить ответ. Это нужно, чтобы:
 *    - CoreTask знал, кому вернуть ack/nak на конкретную команду;
 *    - принятые с шины CAN-кадры рассылались только тем клиентам,
 *      которые сами открыли канал (а не всем подряд).
 *
 *  can_msg_t НЕ несёт client_id -- сам CAN-кадр не привязан к
 *  конкретному клиенту, разметка "кому это разослать" происходит на
 *  уровне CoreTask (по списку клиентов с открытым каналом), а не на
 *  уровне очереди can_to_core_queue.
 */

#ifndef APP_QUEUES_H
#define APP_QUEUES_H

#include "cmsis_os.h"
#include <stdint.h>
#include "can_types.h"

#define ETH_CMD_MAX_LEN    64
#define ETH_RESP_MAX_LEN   64

/*
 * Максимальное число одновременных TCP-клиентов на один CAN-канал.
 * Сейчас на плате один физический CAN-контроллер -- этот лимит
 * относится именно к нему. При добавлении второго физического канала
 * (например, FDCAN2) у него будет свой независимый набор клиентов с
 * тем же лимитом (см. can_channel_t в дальнейших правках can_task.c).
 */
#define MAX_CLIENTS        4U

/*
 * Специальное значение client_id: "нет клиента" / "не адресовано
 * конкретному клиенту". Используется, например, для NetCAN-ответов
 * (N\r, V\r), где client_id команды и так известен из исходного
 * запроса -- отдельного случая с "невалидным" id пока не требуется,
 * но константа зарезервирована на будущее (например, диагностические
 * сообщения не в адрес конкретного клиента).
 */
#define CLIENT_ID_INVALID  0xFFU

typedef struct
{
    uint8_t client_id;              /* какому клиенту принадлежит команда */
    char    data[ETH_CMD_MAX_LEN];
} eth_cmd_msg_t;

typedef struct
{
    uint8_t client_id;              /* какому клиенту доставить ответ */
    char    data[ETH_RESP_MAX_LEN];
} eth_resp_msg_t;

typedef struct
{
    can_frame_t frame;
} can_msg_t;

extern osMessageQueueId_t eth_to_core_queue;
extern osMessageQueueId_t core_to_eth_queue;
extern osMessageQueueId_t core_to_can_queue;
extern osMessageQueueId_t can_to_core_queue;

void AppQueues_Init(void);

#endif /* APP_QUEUES_H */

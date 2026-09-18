/*
 * client_handler.h
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *  Обновлено: поддержка ДВУХ независимых физических CAN-каналов --
 *  ClientHandler_ClientConnected() теперь принимает channel_id (канал,
 *  к которому привязан клиент, задаётся один раз при подключении, по
 *  тому, на какой TCP-порт он пришёл в raw_tcp_server.c).
 */

#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

#include <stdint.h>
#include <stddef.h>

void ClientHandlerTask_Start(void);

void ClientHandler_ClientConnected(uint8_t client_id, uint8_t channel_id);
void ClientHandler_ClientDisconnected(uint8_t client_id);

void ClientHandler_InputBytes(uint8_t client_id, const uint8_t *data, size_t len);
void ClientHandler_PollTx(void);

/*
 * Сколько свободных байт сейчас в RX-кольце этого клиента (там, где
 * накапливаются уже собранные, но ещё не обработанные CoreTask команды).
 * Используется raw_tcp_server.c для управления окном приёма TCP --
 * если места мало, новые данные от клиента не подтверждаются сразу
 * (tcp_recved откладывается), и TCP-стек отправителя сам естественным
 * образом снижает скорость отправки, пока CoreTask/CanTask не разгребут
 * очередь. Возвращает 0 для несуществующего/неактивного клиента.
 */
uint32_t ClientHandler_RxRingFreeBytes(uint8_t client_id);

#endif /* CLIENT_HANDLER_H */

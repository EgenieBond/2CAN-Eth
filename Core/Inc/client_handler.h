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

#endif /* CLIENT_HANDLER_H */

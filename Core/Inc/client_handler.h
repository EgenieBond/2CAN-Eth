/*
 * client_handler.h
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *
 *  Обновлено: поддержка нескольких одновременных TCP-клиентов
 *  (см. MAX_CLIENTS в app_queues.h). Каждый клиент занимает свой слот
 *  0..MAX_CLIENTS-1 -- индекс слота выделяется в raw_tcp_server.c при
 *  приёме подключения (по свободному месту в своём массиве tcp_pcb) и
 *  передаётся сюда как client_id во всех вызовах.
 */

#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

#include <stdint.h>
#include <stddef.h>

void ClientHandlerTask_Start(void);

/*
 * Вызывать из raw_tcp_server.c сразу после того, как клиенту выделен
 * слот (в tcp_server_accept), и сразу при потере клиента (обрыв,
 * штатное закрытие, idle timeout) -- обнуляет буферы именно ЭТОГО
 * слота, не трогая остальных клиентов.
 */
void ClientHandler_ClientConnected(uint8_t client_id);
void ClientHandler_ClientDisconnected(uint8_t client_id);

/* Эти функции были общими и для fake input, и для настоящего TCP;
 * теперь принимают client_id -- чей это входной поток байт. */
void ClientHandler_InputBytes(uint8_t client_id, const uint8_t *data, size_t len);

/*
 * Разгружает core_to_eth_queue и рассылает ответы соответствующим
 * клиентам (каждый eth_resp_msg_t несёт свой client_id) через
 * RawTcpServer_SendAsync(client_id, ...). Обходит все занятые слоты
 * за один вызов -- параметров не требует, т.к. адресат уже записан
 * в каждом сообщении очереди.
 */
void ClientHandler_PollTx(void);

#endif /* CLIENT_HANDLER_H */

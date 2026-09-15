/*
 * raw_tcp_server.h
 *
 *  Created on: Jan 27, 2026
 *      Author: Egenie
 *
 *  Обновлено: поддержка ДВУХ независимых физических CAN-каналов --
 *  два отдельных TCP-порта (2001 -> канал 0, 2002 -> канал 1), общий
 *  "плоский" пул клиентов (MAX_CLIENTS из app_queues.h) на оба порта
 *  сразу. Публичные функции теперь принимают client_id, как и раньше --
 *  канал клиента платой определяется один раз при подключении и не
 *  меняется, вызывающему коду (client_handler.c) знать его не нужно.
 */

#ifndef RAW_TCP_SERVER_H
#define RAW_TCP_SERVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void RawTcpServer_Init(void);
void RawTcpServer_CheckIdleTimeout(void);

int  RawTcpServer_HasClient(uint8_t client_id);

/* Вызывать только из tcpip_thread / raw callbacks */
int  RawTcpServer_Send(uint8_t client_id, const uint8_t *data, size_t len);

/* Безопасно вызывать из обычных FreeRTOS задач */
int  RawTcpServer_SendAsync(uint8_t client_id, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RAW_TCP_SERVER_H */

/*
 * raw_tcp_client.h
 *
 *  Created on: Apr 6, 2026
 *      Author: Egenie
 */

#ifndef RAW_TCP_CLIENT_H
#define RAW_TCP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Запускает FreeRTOS-задачу клиента.
 * Задача сама ждёт поднятия сети и пытается подключиться к NetCAN2.
 */
void RawTcpClientTask_Start(void);

/* Состояние подключения к NetCAN2 */
int  RawTcpClient_IsConnected(void);

/*
 * Отправка в NetCAN2.
 * ВАЖНО: вызывать только из tcpip_thread / raw callbacks.
 */
int  RawTcpClient_Send(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RAW_TCP_CLIENT_H */

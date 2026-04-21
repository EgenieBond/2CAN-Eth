/*
 * raw_tcp_server.h
 *
 *  Created on: Jan 27, 2026
 *      Author: Egenie
 */

#ifndef RAW_TCP_SERVER_H
#define RAW_TCP_SERVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void RawTcpServer_Init(void);
int  RawTcpServer_HasClient(void);

/* Вызывать только из tcpip_thread / raw callbacks */
int  RawTcpServer_Send(const uint8_t *data, size_t len);

/* Безопасно вызывать из обычных FreeRTOS задач */
int  RawTcpServer_SendAsync(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RAW_TCP_SERVER_H */

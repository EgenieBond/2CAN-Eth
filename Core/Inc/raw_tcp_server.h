/*
 * raw_tcp_server.h
 *
 *  Created on: Jan 27, 2026
 *      Author: Egenie
 *
 *  Обновлено: поддержка нескольких одновременных клиентов (MAX_CLIENTS,
 *  см. app_queues.h) -- относится к обычному рабочему режиму
 *  (ETH_BENCHMARK_MODE == 0). Бенчмарк-режимы (1/2/3) остаются
 *  однoклиентскими диагностическими инструментами, как раньше.
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

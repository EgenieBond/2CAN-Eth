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

void RawTcpClientTask_Start(void);
int  RawTcpClient_IsConnected(void);
int  RawTcpClient_Send(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RAW_TCP_CLIENT_H */

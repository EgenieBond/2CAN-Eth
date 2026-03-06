/*
 * client_handler.h
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 */

#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

#include <stdint.h>
#include <stddef.h>

void ClientHandlerTask_Start(void);

/* Эти функции будут общими и для fake input, и для настоящего TCP */
void ClientHandler_InputBytes(const uint8_t *data, size_t len);
void ClientHandler_PollTx(void);

#endif /* INC_CLIENT_HANDLER_H_ */

/*
 * debug_uart.h
 *
 *  Created on: Jan 27, 2026
 *      Author: Egenie
 */


#ifndef INC_DEBUG_UART_H_
#define INC_DEBUG_UART_H_

#ifdef __cplusplus
extern "C" {
#endif

void DebugUART_Init(void);
void DebugUART_InitMutex(void);
void DebugUART_Print(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* INC_DEBUG_UART_H_ */

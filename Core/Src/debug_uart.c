/*
 * debug_uart.c
 *
 *  Created on: Jan 27, 2026
 *      Author: Egenie
 */

/*
 * debug_uart.c
 */

#include "main.h"
#include "debug_uart.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os.h"

extern UART_HandleTypeDef huart3;

/* Mutex для защиты UART printf от параллельных задач */
static osMutexId_t g_uartMutex = NULL;
static const osMutexAttr_t g_uartMutexAttr = { .name = "uartPrintf" };

void DebugUART_InitMutex(void)
{
    if (g_uartMutex == NULL)
    {
        g_uartMutex = osMutexNew(&g_uartMutexAttr);
    }
}

/* Если у тебя уже есть DebugUART_Init — оставь как было.
   Если его нет/не нужен — можно просто оставить пустым. */
void DebugUART_Init(void)
{
    /* optional */
}

void DebugUART_Print(const char *fmt, ...)
{
    /* Локальный буфер (без malloc) */
    char buf[256];

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) return;

    size_t len = (size_t)n;
    if (len >= sizeof(buf))
    {
        /* если обрезало — отправим максимально возможное */
        len = sizeof(buf) - 1;
        buf[len] = '\0';
    }

    if (g_uartMutex) osMutexAcquire(g_uartMutex, osWaitForever);
    HAL_UART_Transmit(&huart3, (uint8_t*)buf, (uint16_t)len, 100);
    if (g_uartMutex) osMutexRelease(g_uartMutex);
}

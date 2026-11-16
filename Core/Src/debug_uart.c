/*
 * debug_uart.c
 *
 *  Created on: Jan 27, 2026
 *      Author: Egenie
 *
 * Отладочный вывод в UART. Уровни вывода и макросы LOG_ERR / LOG_INFO /
 * LOG_DBG -- см. debug_uart.h.
 */

#include "main.h"
#include "debug_uart.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os.h"

extern UART_HandleTypeDef huart3;

/* Текущий уровень вывода (см. debug_uart.h). */
volatile uint8_t g_debug_log_level = (uint8_t)DEBUG_LOG_LEVEL;

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

void DebugUART_Init(void)
{
    /* optional */
}

static void DebugUART_VPrintf(const char *fmt, va_list ap)
{
    /* Локальный буфер (без malloc) */
    char buf[256];

    int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (n < 0) return;

    size_t len = (size_t)n;
    if (len >= sizeof(buf))
    {
        /* если обрезало -- отправим максимально возможное */
        len = sizeof(buf) - 1;
        buf[len] = '\0';
    }

    if (g_uartMutex) osMutexAcquire(g_uartMutex, osWaitForever);
    HAL_UART_Transmit(&huart3, (uint8_t*)buf, (uint16_t)len, 100);
    if (g_uartMutex) osMutexRelease(g_uartMutex);
}

void DebugUART_Printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    DebugUART_VPrintf(fmt, ap);
    va_end(ap);
}

/* Старый интерфейс: печать на уровне INFO */
void DebugUART_Print(const char *fmt, ...)
{
    if ((DEBUG_LOG_LEVEL < LOG_LVL_INFO) || (g_debug_log_level < LOG_LVL_INFO)) { return; }

    va_list ap;
    va_start(ap, fmt);
    DebugUART_VPrintf(fmt, ap);
    va_end(ap);
}

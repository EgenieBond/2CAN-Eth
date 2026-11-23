/*
 * debug_uart.h
 *
 *  Created on: Jan 27, 2026
 *      Author: Egenie
 *
 * Отладочный вывод в UART с УРОВНЯМИ.
 *
 * Каждое сообщение печатается через макрос своего уровня:
 *
 *   LOG_ERR(...)   -- ошибки запуска/инициализации (то, без чего плата не
 *                     работает: не создалась задача, не стартовал FDCAN...);
 *   LOG_INFO(...)  -- события, которые бывают ДО или ПОСЛЕ передачи:
 *                     старт платы, подключение/отключение клиента, открытие/
 *                     закрытие канала, итоговая статистика, журнал ошибок;
 *   LOG_DBG(...)   -- всё, что печатается ВО ВРЕМЯ передачи (heartbeat,
 *                     back-pressure, переполнения, затыки и т.п.).
 *
 * Какие уровни печатаются, задаёт DEBUG_LOG_LEVEL:
 *
 *   LOG_LVL_OFF    (0) -- не печатается вообще ничего;
 *   LOG_LVL_ERROR  (1) -- только LOG_ERR;
 *   LOG_LVL_INFO   (2) -- LOG_ERR + LOG_INFO  (по умолчанию: во время
 *                         передачи UART молчит);
 *   LOG_LVL_DEBUG  (3) -- всё, как было раньше.
 *
 * Как поменять уровень:
 *   1) при сборке -- поменять значение ниже или задать в настройках проекта
 *      (Properties -> C/C++ Build -> Settings -> MCU GCC Compiler ->
 *      Preprocessor: DEBUG_LOG_LEVEL=3). Сообщения выше этого уровня
 *      вообще не попадают в прошивку;
 *   2) во время работы, без пересборки -- переменная g_debug_log_level
 *      (например, в отладчике через Live Expressions). Она может только
 *      ОГРАНИЧИТЬ вывод: больше, чем DEBUG_LOG_LEVEL, она не включит.
 *
 * Старые вызовы DebugUART_Print(...) продолжают работать и считаются
 * уровнем INFO (это обычная функция, она сама проверяет уровень). Поэтому
 * файлы, где печать идёт только при запуске (main.c, ethernet_task.c),
 * менять не нужно.
 *
 * Ошибки, которые случаются во время передачи, не печатаются сразу, а
 * записываются в журнал (event_log.h) и выводятся после закрытия соединения.
 */

#ifndef DEBUG_UART_H
#define DEBUG_UART_H

#include <stdint.h>

#define LOG_LVL_OFF     0
#define LOG_LVL_ERROR   1
#define LOG_LVL_INFO    2
#define LOG_LVL_DEBUG   3

#ifndef DEBUG_LOG_LEVEL
#define DEBUG_LOG_LEVEL LOG_LVL_INFO
#endif

/* Уровень во время работы (можно уменьшить из отладчика). Начальное
 * значение = DEBUG_LOG_LEVEL. */
extern volatile uint8_t g_debug_log_level;

void DebugUART_InitMutex(void);
void DebugUART_Init(void);

/* Сама печать: форматирует строку и отправляет в UART (блокирующе).
 * Напрямую не вызывать -- только через макросы ниже. */
void DebugUART_Printf(const char *fmt, ...);

#define LOG_AT(lvl, ...)                                                    \
    do {                                                                    \
        if ((DEBUG_LOG_LEVEL >= (lvl)) && (g_debug_log_level >= (lvl)))     \
        {                                                                   \
            DebugUART_Printf(__VA_ARGS__);                                  \
        }                                                                   \
    } while (0)

#define LOG_ERR(...)    LOG_AT(LOG_LVL_ERROR, __VA_ARGS__)
#define LOG_INFO(...)   LOG_AT(LOG_LVL_INFO,  __VA_ARGS__)
#define LOG_DBG(...)    LOG_AT(LOG_LVL_DEBUG, __VA_ARGS__)

/* Совместимость со старым кодом: DebugUART_Print печатает на уровне INFO.
 * Это функция, а не макрос, потому что она ещё объявлена в main.h. */
void DebugUART_Print(const char *fmt, ...);

#endif /* DEBUG_UART_H */

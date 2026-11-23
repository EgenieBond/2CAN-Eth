/*
 * event_log.h
 *
 * Журнал ошибок во время передачи.
 *
 * Зачем: печать в UART блокирующая (~10 мс на строку), поэтому во время
 * передачи плата ничего не печатает (см. debug_uart.h). Но знать, КОГДА и
 * КАКИЕ ошибки были, нужно. Поэтому во время работы ошибки только
 * считаются -- быстро, без UART, -- а печатаются списком после того, как
 * закрылось последнее соединение.
 *
 * Как устроено: события группируются по секундам. Запись журнала =
 * (секунда с момента включения платы, источник) + счётчик каждого типа
 * события за эту секунду. Пример вывода:
 *
 *   [EVLOG] t=  151 s  id1: resp_drop=9873 tx_ring_full=1000 tcp_stall=1
 *   [EVLOG] t=  151 s  eth: rx_dma_err=4
 *
 * Время -- в тех же единицах, что tick в остальном логе (tick / 1000).
 *
 * Добавлять событие можно из любой задачи и из прерывания.
 */

#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdint.h>

typedef enum
{
    /* --- ПОТЕРИ направления CAN -> ПК --- */
    EVLOG_CAN_RX_DROP = 0,    /* кадр с шины выброшен: can_to_core_queue полна        (ch)  */
    EVLOG_CORE_TO_ETH_DROP,   /* ответ выброшен: core_to_eth_queue полна               (id)  */
    EVLOG_RESP_DROP,          /* ответ выброшен: tx_batch застрял, pending_resp занят  (id)  */

    /* --- ПОТЕРИ направления ПК -> CAN --- */
    EVLOG_CMD_DROP,           /* команда от ПК выброшена (слишком длинная / таймаут)    (id)  */
    EVLOG_CAN_TX_DROP,        /* кадр не ушёл в шину (TX FIFO завис / AddMessage fail)  (ch)  */

    /* --- признаки затыка (сами по себе НЕ потери) --- */
    EVLOG_TX_RING_FULL,       /* TX-кольцо не приняло пачку, пачка ждёт                 (id)  */
    EVLOG_TCP_STALL,          /* начался эпизод: отправка в ПК встала                   (id)  */

    /* --- потери входящих Ethernet-кадров на плате --- */
    EVLOG_ETH_RX_ALLOC_FAIL,  /* нет свободного буфера в RX pool                        (eth) */
    EVLOG_ETH_RX_DMA_ERR,     /* ошибка DMA приёма (в т.ч. нет свободного дескриптора)  (eth) */
    EVLOG_ETH_RX_HW_DROP,     /* кадр потерян в аппаратуре приёма (MTL/DMA счётчики)    (eth) */

    EVLOG_TYPE_COUNT
} evlog_type_t;

/* Источник события: номер клиента, номер канала или Ethernet в целом */
#define EVLOG_SRC_CLIENT(id)   ((uint8_t)(id))
#define EVLOG_SRC_CHANNEL(ch)  ((uint8_t)(0x10U | (uint8_t)(ch)))
#define EVLOG_SRC_ETH          ((uint8_t)0xFFU)

/* Добавить n событий типа type от источника src (в текущую секунду). */
void EventLog_Add(evlog_type_t type, uint8_t src, uint32_t n);

/* Напечатать журнал и итоги (LOG_INFO) и очистить его.
 * Вызывать, когда передача закончилась (закрылось последнее соединение). */
void EventLog_PrintAndReset(void);

#endif /* EVENT_LOG_H */

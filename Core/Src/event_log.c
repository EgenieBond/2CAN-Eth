/*
 * event_log.c
 *
 * Журнал ошибок во время передачи -- см. event_log.h.
 *
 * Память: EVLOG_MAX_RECORDS записей по ~28 байт (128 записей = ~3.5 КБ в RAM).
 * Одна запись = одна секунда одного источника, в которой было хоть одно
 * событие. Если журнал заполнился, новые секунды уже не записываются, но
 * итоговые суммы по типам всё равно считаются (и печатается, сколько
 * событий не попало в журнал).
 *
 * Ошибки приёма Ethernet (rx_alloc_fail / rx_dma_err / rx_hw_drop) бывают
 * понемногу почти каждую секунду. Если писать их все, журнал заполняется за
 * пару минут (так и было в первом прогоне). Поэтому они попадают в журнал
 * только РЯДОМ с важными событиями (потери, затыки): за ту же секунду и за
 * предыдущую. Остальные учитываются только в итогах ("фон").
 */

#include "event_log.h"
#include "debug_uart.h"
#include "main.h"        /* __get_IPSR() -- проверка "мы в прерывании?" */
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

#define EVLOG_MAX_RECORDS   128U

typedef struct
{
    uint32_t sec;                        /* секунда с момента включения (tick / 1000) */
    uint8_t  src;                        /* EVLOG_SRC_... */
    uint16_t cnt[EVLOG_TYPE_COUNT];      /* событий каждого типа за эту секунду (насыщение на 65535) */
} evlog_rec_t;

static evlog_rec_t g_recs[EVLOG_MAX_RECORDS];
static uint32_t    g_rec_used = 0;
static uint32_t    g_lost_events = 0;                 /* не влезло в журнал */
static uint32_t    g_total[EVLOG_TYPE_COUNT];         /* итоги за всю передачу */

/* ---- "фоновые" события (ошибки приёма Ethernet) ----
 * Копятся здесь по секундам (текущая и предыдущая) и переносятся в журнал,
 * только когда в эту секунду случилось важное событие. */
typedef struct
{
    uint32_t sec;
    uint32_t cnt[EVLOG_TYPE_COUNT];
} evlog_carry_t;

static evlog_carry_t g_carry_cur;               /* фон текущей секунды */
static evlog_carry_t g_carry_prev;              /* фон предыдущей секунды */
static uint32_t      g_last_important_sec = 0xFFFFFFFFU;   /* секунда последнего важного события */

static int EventLog_IsBackground(evlog_type_t type)
{
    return (type == EVLOG_ETH_RX_ALLOC_FAIL) ||
           (type == EVLOG_ETH_RX_DMA_ERR)    ||
           (type == EVLOG_ETH_RX_HW_DROP);
}

static const char *const g_type_name[EVLOG_TYPE_COUNT] =
{
    [EVLOG_CAN_RX_DROP]       = "can_rx_drop",
    [EVLOG_CORE_TO_ETH_DROP]  = "core_to_eth_drop",
    [EVLOG_RESP_DROP]         = "resp_drop",
    [EVLOG_CMD_DROP]          = "cmd_drop",
    [EVLOG_CAN_TX_DROP]       = "can_tx_drop",
    [EVLOG_TX_RING_FULL]      = "tx_ring_full",
    [EVLOG_TCP_STALL]         = "tcp_stall",
    [EVLOG_ETH_RX_ALLOC_FAIL] = "rx_alloc_fail",
    [EVLOG_ETH_RX_DMA_ERR]    = "rx_dma_err",
    [EVLOG_ETH_RX_HW_DROP]    = "rx_hw_drop",
};

static evlog_rec_t *EventLog_FindOrCreate(uint32_t sec, uint8_t src)
{
    /* Записи идут по возрастанию времени -- ищем с конца, только среди
     * записей этой же секунды (их обычно 1-3). */
    for (uint32_t i = g_rec_used; i > 0U; i--)
    {
        evlog_rec_t *r = &g_recs[i - 1U];
        if (r->sec != sec) { break; }
        if (r->src == src) { return r; }
    }

    if (g_rec_used >= EVLOG_MAX_RECORDS) { return NULL; }

    evlog_rec_t *r = &g_recs[g_rec_used++];
    memset(r, 0, sizeof(*r));
    r->sec = sec;
    r->src = src;
    return r;
}

void EventLog_Add(evlog_type_t type, uint8_t src, uint32_t n)
{
    if (((uint32_t)type >= (uint32_t)EVLOG_TYPE_COUNT) || (n == 0U)) { return; }

    uint32_t sec = osKernelGetTickCount() / 1000U;   /* можно и из прерывания */
    uint8_t  in_isr = (__get_IPSR() != 0U) ? 1U : 0U;
    UBaseType_t isr_state = 0;

    /* Короткая критическая секция: событие могут добавлять сразу несколько
     * задач и прерывание FDCAN. Внутри -- только поиск по нескольким
     * записям и сложение, никакого UART. */
    if (in_isr) { isr_state = taskENTER_CRITICAL_FROM_ISR(); }
    else        { taskENTER_CRITICAL(); }

    g_total[type] += n;

    if (EventLog_IsBackground(type) && (sec != g_last_important_sec))
    {
        /* Фон: в эту секунду пока ничего важного не было -- только копим. */
        if (g_carry_cur.sec != sec)
        {
            if (g_carry_cur.sec + 1U == sec) { g_carry_prev = g_carry_cur; }
            else                             { memset(&g_carry_prev, 0, sizeof(g_carry_prev)); }
            memset(&g_carry_cur, 0, sizeof(g_carry_cur));
            g_carry_cur.sec = sec;
        }
        g_carry_cur.cnt[type] += n;
    }
    else
    {
        if (!EventLog_IsBackground(type) && (sec != g_last_important_sec))
        {
            /* Первое важное событие в этой секунде: сначала переносим в журнал
             * фон предыдущей и этой секунды (если он был). */
            g_last_important_sec = sec;
            const evlog_carry_t *carry[2] = { &g_carry_prev, &g_carry_cur };
            for (uint32_t k = 0; k < 2U; k++)
            {
                const evlog_carry_t *c = carry[k];
                if ((c->sec + 1U != sec) && (c->sec != sec)) { continue; }
                for (uint32_t t = 0; t < (uint32_t)EVLOG_TYPE_COUNT; t++)
                {
                    if (c->cnt[t] == 0U) { continue; }
                    evlog_rec_t *er = EventLog_FindOrCreate(c->sec, EVLOG_SRC_ETH);
                    if (er == NULL) { g_lost_events += c->cnt[t]; continue; }
                    uint32_t v = (uint32_t)er->cnt[t] + c->cnt[t];
                    er->cnt[t] = (v > 0xFFFFU) ? (uint16_t)0xFFFFU : (uint16_t)v;
                }
            }
            memset(&g_carry_prev, 0, sizeof(g_carry_prev));
            memset(&g_carry_cur, 0, sizeof(g_carry_cur));
            g_carry_cur.sec = sec;
        }

        evlog_rec_t *r = EventLog_FindOrCreate(sec, src);
        if (r == NULL)
        {
            g_lost_events += n;
        }
        else
        {
            uint32_t v = (uint32_t)r->cnt[type] + n;
            r->cnt[type] = (v > 0xFFFFU) ? (uint16_t)0xFFFFU : (uint16_t)v;
        }
    }

    if (in_isr) { taskEXIT_CRITICAL_FROM_ISR(isr_state); }
    else        { taskEXIT_CRITICAL(); }
}

static void EventLog_SrcName(uint8_t src, char *out, uint32_t out_len)
{
    if (src == EVLOG_SRC_ETH)        { snprintf(out, out_len, "eth"); }
    else if ((src & 0x10U) != 0U)    { snprintf(out, out_len, "ch%u", (unsigned)(src & 0x0FU)); }
    else                             { snprintf(out, out_len, "id%u", (unsigned)src); }
}

void EventLog_PrintAndReset(void)
{
    /* Печатаем прямо из журнала, без копии (экономим RAM): вызывается,
     * когда все соединения уже закрыты, новых событий почти не бывает.
     * Если какое-то всё же добавится во время печати -- не страшно,
     * журнал всё равно очищается ниже. */
    uint32_t used = g_rec_used;

    LOG_INFO("\r\n==================== EVENT LOG (ошибки во время передачи) ====================\r\n");

    if (used == 0U)
    {
        LOG_INFO("[EVLOG] событий не было\r\n");
    }

    for (uint32_t i = 0; i < used; i++)
    {
        const evlog_rec_t *r = &g_recs[i];
        char line[200];
        char src[8];
        int  pos;

        EventLog_SrcName(r->src, src, sizeof(src));
        pos = snprintf(line, sizeof(line), "[EVLOG] t=%5lu s  %-4s:", (unsigned long)r->sec, src);

        for (uint32_t t = 0; (t < (uint32_t)EVLOG_TYPE_COUNT) && (pos > 0) && (pos < (int)sizeof(line)); t++)
        {
            if (r->cnt[t] == 0U) { continue; }
            pos += snprintf(&line[pos], sizeof(line) - (uint32_t)pos, " %s=%u%s",
                            g_type_name[t], (unsigned)r->cnt[t],
                            (r->cnt[t] == 0xFFFFU) ? "+" : "");
        }
        LOG_INFO("%s\r\n", line);
    }

    if (g_lost_events != 0U)
    {
        LOG_INFO("[EVLOG] журнал заполнен (%u записей): ещё %lu событий не попали в список, "
                 "но учтены в итогах\r\n",
                 (unsigned)EVLOG_MAX_RECORDS, (unsigned long)g_lost_events);
    }

    LOG_INFO("[EVLOG] (ошибки приёма Ethernet показаны только рядом с потерями/затыками; "
             "все остальные -- только в итогах)\r\n");
    LOG_INFO("[EVLOG] ИТОГО:");
    for (uint32_t t = 0; t < (uint32_t)EVLOG_TYPE_COUNT; t++)
    {
        if (g_total[t] != 0U)
        {
            LOG_INFO(" %s=%lu", g_type_name[t], (unsigned long)g_total[t]);
        }
    }
    LOG_INFO("\r\n==============================================================================\r\n\r\n");

    taskENTER_CRITICAL();
    g_rec_used = 0;
    g_lost_events = 0;
    memset(g_total, 0, sizeof(g_total));
    memset(&g_carry_cur, 0, sizeof(g_carry_cur));
    memset(&g_carry_prev, 0, sizeof(g_carry_prev));
    g_last_important_sec = 0xFFFFFFFFU;
    taskEXIT_CRITICAL();
}

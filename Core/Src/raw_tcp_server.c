/*
 * raw_tcp_server.c
 *
 * TCP RAW server -- ДВА независимых порта (по одному на физический
 * CAN-канал), общий "плоский" пул клиентов на оба порта сразу.
 *
 * Порт 2001 -> канал 0 (FDCAN1)
 * Порт 2002 -> канал 1 (FDCAN2)
 *
 * Один и тот же клиент (программа на ПК) может открыть ДВА отдельных
 * TCP-соединения -- к порту 2001 и к порту 2002 -- и работать с обеими
 * шинами одновременно и независимо; для платы это просто два разных
 * слота в общем пуле, каждый со своим channel_id.
 *
 * ===== RX FLOW CONTROL =====
 * Входящие pbuf'ы НЕ удерживаются: данные сразу копируются в RX-кольцо
 * клиента (client_handler.c), pbuf тут же освобождается. Это важно: в
 * zero-copy драйвере pbuf -- это DMA-буфер Ethernet, их всего 20 на ВЕСЬ
 * входящий трафик. Удерживать их = душить вход всей сети, причём число
 * удержанных буферов зависит от размера сегментов клиента (мелкие
 * сегменты -> много буферов при том же числе байт).
 *
 * Торможение ПК делается так: tcp_recved() вызывается не при приёме, а
 * когда команда реально ушла из RX-кольца дальше по конвейеру (кредит,
 * накопленный в client_handler.c). Пока конвейер стоит (CAN не успевает),
 * кредита нет -> окно не открывается -> ПК ждёт. Неподтверждённых байт на
 * плате никогда не больше TCP_WND, и кольцо рассчитано на этот объём
 * (проверяется _Static_assert в client_handler.c).
 *
 * Прочее:
 *  - страховка от потерянного пробуждения TX: если TX-кольцо клиента
 *    полно, RawTcpServer_SendAsync всё равно планирует flush;
 *  - idle timeout не вызывает HandleClientGone дважды;
 *  - счётчики сбрасываются при новом подключении.
 */

#include "raw_tcp_server.h"
#include "client_handler.h"
#include "core_task.h"
#include "app_queues.h"   /* MAX_CLIENTS, NUM_CHANNELS */

#include "lwip/tcp.h"
#include "lwip/inet.h"
#include "lwip/tcpip.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/memp.h"

#include "main.h"     /* регистры ETH (CMSIS) -- для аппаратных счётчиков потерь приёма */

#include "cmsis_os.h"
#include "debug_uart.h"
#include "event_log.h"

#include <string.h>
#include <stdint.h>

/* Порт для каждого канала: индекс = channel_id */
static const uint16_t TCP_SERVER_PORT[NUM_CHANNELS] = { 2001U, 2002U };

/* TX-кольцо: буфер ответов в ПК (CAN -> ETH). Это направление нельзя
 * притормозить: узлы на шине не ждут. Если отправка в ПК встала (потерялся
 * пакет, TCP ждёт повтора), кадры с шины копятся здесь.
 *
 * Полная шина 1 Мбит/с = ~7200 кадров/с x 27 байт = ~194 КБ/с на клиента.
 *   4 КБ  -> ~20 мс  (было раньше);
 *   16 КБ -> ~80 мс  (сейчас; паузы после потери пакета ~80-140 мс).
 *
 * Кольца лежат в DTCM (секция .lwip_dtcm, см. линкер-скрипт): к ним
 * обращается только процессор (memcpy из ClientHandler и tcp_write COPY),
 * Ethernet-DMA их не трогает -- поэтому DTCM подходит. В RAM_D1 места нет.
 * Размер: MAX_CLIENTS x 16 КБ = 64 КБ из ~95 КБ свободных в DTCM (остаток
 * -- запас на увеличение кучи lwIP при 4 клиентах). Если не влезет,
 * линкер выдаст ошибку переполнения DTCMRAM. */
#define RAW_TCP_TX_RING_SIZE_PER_CLIENT   (16U * 1024U)
#define CLIENT_IDLE_TIMEOUT_MS             (10UL * 1000UL)

/* Счётчики потерь входящих кадров Ethernet (см. RawTcp_PollEthRxDrops) */
typedef struct
{
    uint32_t rx_alloc_fail;
    uint32_t dma_err;
    uint32_t hw_missed;
    uint32_t hw_ovf;
    uint32_t hw_mis;
} eth_rx_drops_t;

/* Один записанный эпизод затыка отправки к ПК (см. RawTcp_TrackStall).
 * Эпизоды НЕ печатаются в момент события: печать в UART из tcpip-потока
 * блокирует его на миллисекунды, а именно в эти моменты нагрузка
 * максимальна -- входящие кадры копятся, RX-буферы драйвера (их 13)
 * кончаются, и плата начинает терять кадры от ПК. Поэтому эпизоды
 * пишутся в память и печатаются списком при закрытии соединения. */
#define RAW_TCP_STALL_LOG_LEN   16U

typedef struct
{
    uint32_t start_tick;
    uint32_t dur_ms;
    uint32_t min_snd_wnd;
    uint16_t max_queuelen;
    uint8_t  max_nrtx;
    uint32_t write_err_mem;
    eth_rx_drops_t drops;        /* приращения счётчиков потерь приёма за эпизод */
} stall_rec_t;

typedef struct
{
    struct tcp_pcb *pcb;
    uint8_t          channel_id;   /* канал, к которому привязан этот клиент -- задаётся при accept, не меняется */
    uint32_t         last_activity_tick;

    /* сами байты кольца -- в g_tx_ring[id] (DTCM), здесь только индексы */
    volatile uint32_t tx_head;
    volatile uint32_t tx_tail;
    volatile uint8_t  tx_flush_scheduled;

    uint32_t tx_drop_count;
    uint32_t tcp_err_mem_count;      /* tcp_write вернул ERR_MEM */
    uint32_t tcp_output_mem_count;   /* tcp_output вернул ERR_MEM (нет свободного ETH TX-слота) */

    /* RX flow control: сколько байт принято, но ещё не подтверждено
     * через tcp_recved(). Меняется только в tcpip-потоке. Не может
     * превышать TCP_WND -- это и есть "размер торможения". */
    uint32_t rx_unacked;
    uint32_t rx_unacked_max;   /* диагностика */

    /* Диагностика "затыков" отправки к ПК (CAN -> ETH). Эпизод начинается,
     * когда TX-кольцо заполнено больше чем на 3/4, и заканчивается, когда
     * опускается ниже 1/4. Всё пишется и читается только в tcpip-потоке
     * (RawTcp_TryFlush), поэтому поля pcb читать здесь безопасно. */
    uint8_t  stall_active;
    uint32_t stall_start_tick;
    uint32_t stall_count;
    uint32_t stall_max_ms;
    uint32_t stall_min_snd_wnd;      /* минимальное окно, объявленное ПК (0 = ПК не читает сокет) */
    uint16_t stall_max_queuelen;     /* максимум сегментов в очереди отправки lwIP */
    uint8_t  stall_max_nrtx;         /* максимум повторных передач (>0 = сегмент потерялся в сети) */
    uint32_t stall_write_err_mem;    /* tcp_write ERR_MEM за время эпизода */
    eth_rx_drops_t stall_drops_start; /* счётчики потерь приёма ETH в начале эпизода */

    stall_rec_t stall_log[RAW_TCP_STALL_LOG_LEN];   /* первые эпизоды -- печатаются при закрытии */
    uint32_t    stall_log_used;                     /* сколько записей занято */
} tcp_client_t;

static struct tcp_pcb *g_server_pcb[NUM_CHANNELS] = { NULL };
static tcp_client_t g_clients[MAX_CLIENTS];

/* TX-кольца всех клиентов -- в DTCM (NOLOAD-секция: не обнуляется при старте,
 * это не нужно -- индексы head/tail сбрасываются при каждом подключении). */
static uint8_t g_tx_ring[MAX_CLIENTS][RAW_TCP_TX_RING_SIZE_PER_CLIENT]
    __attribute__((section(".lwip_dtcm"), aligned(32)));

/* ===== Счётчики потерь ВХОДЯЩИХ кадров Ethernet =====
 *
 * Нужны, чтобы понять, не теряет ли плата кадры от ПК (ACK получателя,
 * данные отправителя). Если теряет, lwIP не видит подтверждений и ждёт
 * таймера повторной передачи (RTO, ~1-1.5 с) -- ровно такие затыки и
 * видны в логе (dur ~1200 мс, nrtx=1, оба соединения сразу).
 *
 *   rx_alloc_fail -- драйвер не смог выделить RX-буфер (пул RX_POOL пуст);
 *   dma_err       -- ошибки DMA из HAL_ETH_ErrorCallback (в т.ч. RBU);
 *   hw_missed     -- DMACMFCR.MFC: кадры, потерянные DMA из-за отсутствия
 *                    свободного дескриптора;
 *   hw_ovf        -- MTLRQMPOCR.OVFPKTCNT: кадры, выброшенные из-за
 *                    переполнения приёмной FIFO MTL;
 *   hw_mis        -- MTLRQMPOCR.MISPKTCNT: кадры, выброшенные приёмной
 *                    очередью MTL.
 * Аппаратные регистры сбрасываются при чтении и насыщаются на 2047,
 * поэтому опрашиваем их часто (из RawTcp_TrackStall, ~раз в мс) и
 * накапливаем в 32-битных суммах. */
extern volatile uint32_t g_rx_alloc_fail_cnt;
extern volatile uint32_t g_eth_dma_error_count;

#if LWIP_STATS && MEMP_STATS
/* Пул RX-буферов драйвера (LWIP_MEMPOOL_DECLARE(RX_POOL, ...) в ethernetif.c) */
extern const struct memp_desc memp_RX_POOL;
#endif

static eth_rx_drops_t g_eth_rx_drops;

static void RawTcp_PollEthRxDrops(void)
{
    uint32_t mfc   = ETH->DMACMFCR;
    uint32_t mtl   = ETH->MTLRQMPOCR;
    uint32_t alloc = g_rx_alloc_fail_cnt;
    uint32_t dma   = g_eth_dma_error_count;

    uint32_t d_missed = (mfc & 0x7FFU);
    uint32_t d_ovf    = (mtl & 0x7FFU);
    uint32_t d_mis    = ((mtl >> 16) & 0x7FFU);

    /* Приращения -- в журнал ошибок (event_log.c), с привязкой к секунде.
     * EventLog_Add ничего не делает, если приращение 0. */
    EventLog_Add(EVLOG_ETH_RX_ALLOC_FAIL, EVLOG_SRC_ETH, alloc - g_eth_rx_drops.rx_alloc_fail);
    EventLog_Add(EVLOG_ETH_RX_DMA_ERR,    EVLOG_SRC_ETH, dma   - g_eth_rx_drops.dma_err);
    EventLog_Add(EVLOG_ETH_RX_HW_DROP,    EVLOG_SRC_ETH, d_missed + d_ovf + d_mis);

    g_eth_rx_drops.hw_missed    += d_missed;
    g_eth_rx_drops.hw_ovf       += d_ovf;
    g_eth_rx_drops.hw_mis       += d_mis;
    g_eth_rx_drops.rx_alloc_fail = alloc;
    g_eth_rx_drops.dma_err       = dma;
}

/* ===== ИСКУССТВЕННАЯ ЗАДЕРЖКА tcpip-ПОТОКА (для воспроизведения ошибки) =====
 *
 * Гипотеза: заморозки начинаются, когда tcpip-поток на какое-то время не
 * успевает разбирать входящие пакеты -> кончаются RX-буферы -> теряются
 * пакеты от ПК (в т.ч. ACK) -> TCP ждёт таймер повтора.
 *
 * Чтобы проверить это и сделать ошибку частой, можно раз в
 * RAW_TCP_STALL_INJECT_PERIOD_MS миллисекунд специально "занять" tcpip-поток
 * на RAW_TCP_STALL_INJECT_MS миллисекунд (пустой цикл, процессор не отдаётся
 * задачам того же и более низкого приоритета).
 *
 *   RAW_TCP_STALL_INJECT_MS = 0   -- выключено (обычная работа);
 *   RAW_TCP_STALL_INJECT_MS = 50  -- для теста: задержка 50 мс раз в секунду.
 *
 * Если с задержкой заморозки начнутся почти каждую секунду -- гипотеза
 * подтверждена, и на этом режиме можно быстро (за 5 минут, а не за 30)
 * проверять исправления. */
#ifndef RAW_TCP_STALL_INJECT_MS
#define RAW_TCP_STALL_INJECT_MS          0U
#endif
#ifndef RAW_TCP_STALL_INJECT_PERIOD_MS
#define RAW_TCP_STALL_INJECT_PERIOD_MS   1000U
#endif

static uint32_t g_inject_count = 0;

static void RawTcp_MaybeInjectStall(void)
{
#if (RAW_TCP_STALL_INJECT_MS > 0U)
    static uint32_t last_tick = 0;
    uint32_t now = osKernelGetTickCount();

    if ((now - last_tick) < RAW_TCP_STALL_INJECT_PERIOD_MS) { return; }
    last_tick = now;
    g_inject_count++;

    /* Ждём, не отдавая процессор (tick растёт по прерыванию SysTick). */
    while ((osKernelGetTickCount() - now) < RAW_TCP_STALL_INJECT_MS)
    {
        __NOP();
    }
#endif
}

/* ===== Диагностика ===== */

static void RawTcp_PrintClientStats(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];

    LOG_INFO("[TCP] id=%u TX drop count=%lu ERR_MEM count=%lu tcp_output ERR_MEM=%lu "
                    "rx_unacked max=%lu (TCP_WND=%lu) TX stalls=%lu (max %lu ms)\r\n",
                    (unsigned)id,
                    (unsigned long)c->tx_drop_count,
                    (unsigned long)c->tcp_err_mem_count,
                    (unsigned long)c->tcp_output_mem_count,
                    (unsigned long)c->rx_unacked_max,
                    (unsigned long)TCP_WND,
                    (unsigned long)c->stall_count,
                    (unsigned long)c->stall_max_ms);

#if LWIP_STATS && MEM_STATS && MEMP_STATS
    /* Общие ресурсы lwIP (на все соединения, накопительно с момента старта):
     *   heap    -- куча MEM_SIZE: здесь лежат копии данных отправки
     *              (tcp_write COPY) и пакеты ACK. err > 0 -- кучи не хватило;
     *   tcp_seg -- пул структур сегментов MEMP_NUM_TCP_SEG. err > 0 -- не хватило.
     * max -- пиковое использование, avail -- сколько всего есть. */
    LOG_INFO("[LWIP] heap: max=%lu of %lu err=%lu | tcp_seg: max=%lu of %lu err=%lu\r\n",
                    (unsigned long)lwip_stats.mem.max,
                    (unsigned long)lwip_stats.mem.avail,
                    (unsigned long)lwip_stats.mem.err,
                    (unsigned long)lwip_stats.memp[MEMP_TCP_SEG]->max,
                    (unsigned long)lwip_stats.memp[MEMP_TCP_SEG]->avail,
                    (unsigned long)lwip_stats.memp[MEMP_TCP_SEG]->err);

    /* Кто держит RX-буферы Ethernet:
     *   rx_pool max     -- пик одновременно занятых RX-буферов драйвера
     *                      (из ETH_RX_BUFFER_CNT); err -- сколько раз пул
     *                      был пуст (то же, что alloc_fail);
     *   tcpip_inpkt max -- пик кадров, стоявших в очереди к tcpip-потоку
     *                      одновременно (каждый держит один RX-буфер).
     * Если tcpip_inpkt max почти равен rx_pool max -- буферы держит очередь
     * к tcpip-потоку, то есть tcpip-поток в эти моменты не успевал. */
    LOG_INFO("[LWIP] rx_pool: max=%lu of %lu err=%lu | tcpip_inpkt: max=%lu of %lu err=%lu\r\n",
                    (unsigned long)memp_RX_POOL.stats->max,
                    (unsigned long)memp_RX_POOL.stats->avail,
                    (unsigned long)memp_RX_POOL.stats->err,
                    (unsigned long)lwip_stats.memp[MEMP_TCPIP_MSG_INPKT]->max,
                    (unsigned long)lwip_stats.memp[MEMP_TCPIP_MSG_INPKT]->avail,
                    (unsigned long)lwip_stats.memp[MEMP_TCPIP_MSG_INPKT]->err);
#endif

    if (RAW_TCP_STALL_INJECT_MS > 0U)
    {
        LOG_INFO("[TEST] искусственная задержка tcpip-потока: %u мс раз в %u мс, сделано %lu раз\r\n",
                 (unsigned)RAW_TCP_STALL_INJECT_MS, (unsigned)RAW_TCP_STALL_INJECT_PERIOD_MS,
                 (unsigned long)g_inject_count);
    }

    RawTcp_PollEthRxDrops();
    LOG_INFO("[ETH] RX drops total since boot: alloc_fail=%lu dma_err=%lu hw_missed=%lu hw_ovf=%lu hw_mis=%lu\r\n",
                    (unsigned long)g_eth_rx_drops.rx_alloc_fail,
                    (unsigned long)g_eth_rx_drops.dma_err,
                    (unsigned long)g_eth_rx_drops.hw_missed,
                    (unsigned long)g_eth_rx_drops.hw_ovf,
                    (unsigned long)g_eth_rx_drops.hw_mis);

    /* Эпизоды затыков, записанные во время работы (печать здесь, при
     * закрытии, когда нагрузки уже нет). */
    for (uint32_t i = 0; i < c->stall_log_used; i++)
    {
        const stall_rec_t *r = &c->stall_log[i];
        LOG_INFO("[TCP] id=%u stall #%lu tick=%lu dur=%lu ms: min snd_wnd(PC)=%lu "
                        "max queuelen=%u max nrtx=%u write ERR_MEM=%lu | "
                        "ETH RX drops: alloc_fail=%lu dma_err=%lu hw_missed=%lu hw_ovf=%lu hw_mis=%lu\r\n",
                        (unsigned)id, (unsigned long)(i + 1U),
                        (unsigned long)r->start_tick, (unsigned long)r->dur_ms,
                        (unsigned long)r->min_snd_wnd, (unsigned)r->max_queuelen,
                        (unsigned)r->max_nrtx, (unsigned long)r->write_err_mem,
                        (unsigned long)r->drops.rx_alloc_fail, (unsigned long)r->drops.dma_err,
                        (unsigned long)r->drops.hw_missed, (unsigned long)r->drops.hw_ovf,
                        (unsigned long)r->drops.hw_mis);
    }
    if (c->stall_count > c->stall_log_used)
    {
        LOG_INFO("[TCP] id=%u ... ещё %lu эпизодов не записаны (журнал на %u)\r\n",
                        (unsigned)id, (unsigned long)(c->stall_count - c->stall_log_used),
                        (unsigned)RAW_TCP_STALL_LOG_LEN);
    }
}

/* ===== TX-кольцо конкретного клиента ===== */

static uint32_t RawTcp_TxUsed(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];
    if (c->tx_head >= c->tx_tail) { return c->tx_head - c->tx_tail; }
    return RAW_TCP_TX_RING_SIZE_PER_CLIENT - c->tx_tail + c->tx_head;
}

static uint32_t RawTcp_TxFree(uint8_t id)
{
    return RAW_TCP_TX_RING_SIZE_PER_CLIENT - RawTcp_TxUsed(id) - 1U;
}

static void RawTcp_TxReset(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];
    c->tx_head            = 0;
    c->tx_tail            = 0;
    c->tx_flush_scheduled = 0;
}

static int RawTcp_TxPush(uint8_t id, const uint8_t *data, size_t len)
{
    tcp_client_t *c = &g_clients[id];

    if ((data == NULL) || (len == 0U)) { return -1; }

    osKernelLock();

    if (RawTcp_TxFree(id) < len)
    {
        osKernelUnlock();
        c->tx_drop_count++;
        EventLog_Add(EVLOG_TX_RING_FULL, EVLOG_SRC_CLIENT(id), 1U);
        /* Это не потеря данных: батч остаётся у ClientHandler и будет
         * предложен снова. Печатаем редко, чтобы не душить систему UART'ом. */
        if ((c->tx_drop_count == 1U) || ((c->tx_drop_count % 1000U) == 0U))
        {
            LOG_DBG("[TCP] id=%u TX ring full (push refused) count=%lu\r\n",
                            (unsigned)id, (unsigned long)c->tx_drop_count);
        }
        return -2;
    }

    for (size_t i = 0; i < len; i++)
    {
        g_tx_ring[id][c->tx_head] = data[i];
        c->tx_head = (c->tx_head + 1U) % RAW_TCP_TX_RING_SIZE_PER_CLIENT;
    }

    osKernelUnlock();
    return 0;
}

static uint32_t RawTcp_TxContiguousLen(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];
    if (c->tx_head == c->tx_tail) { return 0; }
    if (c->tx_head > c->tx_tail)  { return c->tx_head - c->tx_tail; }
    return RAW_TCP_TX_RING_SIZE_PER_CLIENT - c->tx_tail;
}

static void RawTcp_TxConsume(uint8_t id, uint32_t len)
{
    tcp_client_t *c = &g_clients[id];
    c->tx_tail = (c->tx_tail + len) % RAW_TCP_TX_RING_SIZE_PER_CLIENT;
}

/*
 * Отслеживает эпизоды, когда отправка к ПК не успевает (TX-кольцо почти
 * полно), и по окончании эпизода печатает ОДНУ строку с причиной:
 *
 *   min snd_wnd = 0      -> ПК перестал читать сокет (окно ПК закрыто),
 *                           причина на стороне ПК;
 *   max nrtx > 0         -> сегмент потерялся в сети и lwIP ждал повтора;
 *   ни то, ни другое,
 *   но write ERR_MEM > 0 -> упёрлись в очередь отправки самого lwIP
 *                           (TCP_SND_QUEUELEN / MEMP_NUM_TCP_SEG / MEM_SIZE).
 *
 * Вызывается только из tcpip-потока (RawTcp_TryFlush), поэтому поля pcb
 * читать здесь безопасно. Здесь НИЧЕГО не печатается: эпизод только
 * записывается в c->stall_log, а печатается при закрытии соединения
 * (RawTcp_PrintClientStats) -- печать в UART из tcpip-потока в момент
 * пиковой нагрузки сама вызывала исчерпание RX-буферов.
 */
static void RawTcp_TrackStall(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];
    uint32_t used = RawTcp_TxUsed(id);
    uint32_t now  = osKernelGetTickCount();

    RawTcp_PollEthRxDrops();

    if (!c->stall_active)
    {
        if (used > ((RAW_TCP_TX_RING_SIZE_PER_CLIENT * 3U) / 4U))
        {
            c->stall_active        = 1;
            c->stall_start_tick    = now;
            c->stall_min_snd_wnd   = (uint32_t)c->pcb->snd_wnd;
            c->stall_max_queuelen  = (uint16_t)c->pcb->snd_queuelen;
            c->stall_max_nrtx      = (uint8_t)c->pcb->nrtx;
            c->stall_write_err_mem = 0;
            c->stall_drops_start   = g_eth_rx_drops;
            EventLog_Add(EVLOG_TCP_STALL, EVLOG_SRC_CLIENT(id), 1U);
        }
        return;
    }

    /* эпизод идёт -- обновляем экстремумы */
    if ((uint32_t)c->pcb->snd_wnd < c->stall_min_snd_wnd) { c->stall_min_snd_wnd  = (uint32_t)c->pcb->snd_wnd; }
    if (c->pcb->snd_queuelen > c->stall_max_queuelen)     { c->stall_max_queuelen = (uint16_t)c->pcb->snd_queuelen; }
    if (c->pcb->nrtx > c->stall_max_nrtx)                 { c->stall_max_nrtx     = (uint8_t)c->pcb->nrtx; }

    if (used < (RAW_TCP_TX_RING_SIZE_PER_CLIENT / 4U))
    {
        uint32_t dur = now - c->stall_start_tick;

        c->stall_active = 0;
        c->stall_count++;
        if (dur > c->stall_max_ms) { c->stall_max_ms = dur; }

        if (c->stall_log_used < RAW_TCP_STALL_LOG_LEN)
        {
            stall_rec_t *r = &c->stall_log[c->stall_log_used++];
            r->start_tick          = c->stall_start_tick;
            r->dur_ms              = dur;
            r->min_snd_wnd         = c->stall_min_snd_wnd;
            r->max_queuelen        = c->stall_max_queuelen;
            r->max_nrtx            = c->stall_max_nrtx;
            r->write_err_mem       = c->stall_write_err_mem;
            r->drops.rx_alloc_fail = g_eth_rx_drops.rx_alloc_fail - c->stall_drops_start.rx_alloc_fail;
            r->drops.dma_err       = g_eth_rx_drops.dma_err       - c->stall_drops_start.dma_err;
            r->drops.hw_missed     = g_eth_rx_drops.hw_missed     - c->stall_drops_start.hw_missed;
            r->drops.hw_ovf        = g_eth_rx_drops.hw_ovf        - c->stall_drops_start.hw_ovf;
            r->drops.hw_mis        = g_eth_rx_drops.hw_mis        - c->stall_drops_start.hw_mis;
        }
    }
}

/*
 * Перекладывает данные из TX-кольца клиента в lwIP (tcp_write) и
 * отправляет их (tcp_output).
 *
 * Сначала кладём в lwIP ВСЁ, что влезает (несколько tcp_write подряд),
 * и только потом ОДИН tcp_output: так lwIP собирает данные в сегменты
 * максимального размера, а не шлёт по кадру на каждый кусок -- меньше
 * кадров Ethernet, меньше нагрузка на 6 TX-слотов драйвера.
 *
 * tcp_output может вернуть ERR_MEM, когда все TX-слоты Ethernet заняты
 * (low_level_output в ethernetif.c). Это НЕ потеря: данные уже лежат в
 * очереди pcb (unsent), lwIP отправит их при следующем tcp_output -- из
 * следующего flush, из sent-колбэка или при приходе ACK. Поэтому здесь
 * только считаем, а не печатаем: печать каждого случая блокировала
 * tcpip-поток на время вывода в UART, из-за этого TX-слоты освобождались
 * ещё медленнее, и возникала положительная обратная связь.
 */
static void RawTcp_TryFlush(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];

    if (c->pcb == NULL) { RawTcp_TxReset(id); return; }

    for (;;)
    {
        uint32_t available = RawTcp_TxContiguousLen(id);
        if (available == 0U) { break; }

        u16_t sndbuf = tcp_sndbuf(c->pcb);
        if (sndbuf == 0U)   { break; }

        uint32_t to_send = available;
        if (to_send > sndbuf) { to_send = sndbuf; }
        if (to_send > 0xFFFFU) { to_send = 0xFFFFU; }
        if (to_send == 0U)    { break; }

        err_t wr = tcp_write(c->pcb, &g_tx_ring[id][c->tx_tail], (u16_t)to_send, TCP_WRITE_FLAG_COPY);

        if (wr == ERR_MEM)
        {
            /* Нехватка памяти/сегментов lwIP. Повтор -- из sent-колбэка
             * (если есть неподтверждённые данные) или из следующего
             * RawTcpServer_SendAsync (он планирует flush даже при полном
             * кольце). */
            c->tcp_err_mem_count++;
            if (c->stall_active) { c->stall_write_err_mem++; }
            break;
        }

        if (wr != ERR_OK)
        {
            LOG_DBG("[TCP] id=%u tcp_write err=%d\r\n", (unsigned)id, (int)wr);
            break;
        }

        RawTcp_TxConsume(id, to_send);
    }

    RawTcp_TrackStall(id);

    /* Один tcp_output на всё накопленное -- и заодно повтор того, что
     * осталось неотправленным с прошлого раза. */
    if (c->pcb->unsent != NULL)
    {
        err_t out = tcp_output(c->pcb);
        if (out == ERR_MEM)
        {
            c->tcp_output_mem_count++;
        }
        else if (out != ERR_OK)
        {
            static uint32_t other_err_count = 0;
            other_err_count++;
            if ((other_err_count == 1U) || ((other_err_count % 1000U) == 0U))
            {
                LOG_DBG("[TCP] id=%u tcp_output err=%d (count=%lu)\r\n",
                                (unsigned)id, (int)out, (unsigned long)other_err_count);
            }
        }
    }
}

static void raw_tcp_flush_cb(void *arg)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;
    if (id >= MAX_CLIENTS) { return; }
    g_clients[id].tx_flush_scheduled = 0;
    RawTcp_MaybeInjectStall();
    RawTcp_TryFlush(id);
}

static void RawTcp_ScheduleFlush(uint8_t id)
{
    tcp_client_t *c = &g_clients[id];

    if (c->pcb == NULL)        { return; }
    if (c->tx_flush_scheduled) { return; }
    c->tx_flush_scheduled = 1;

    err_t cb_err = tcpip_callback(raw_tcp_flush_cb, (void*)(uintptr_t)id);
    if (cb_err != ERR_OK)
    {
        c->tx_flush_scheduled = 0;
        LOG_DBG("[TCP] id=%u tcpip_callback(flush) err=%d\r\n", (unsigned)id, (int)cb_err);
    }
}

/* ===== RX FLOW CONTROL: подтверждение принятых байт ===== */

/* Один общий флаг, чтобы не заваливать tcpip_callback() повторными
 * запросами, пока предыдущий ещё не отработал. */
static volatile uint8_t g_recved_scheduled = 0;
static uint32_t g_recved_cb_fail = 0;

/* Выполняется в tcpip-потоке: забирает у ClientHandler кредит каждого
 * клиента и подтверждает ровно столько байт -- TCP-окно открывается на
 * столько, сколько реально ушло дальше по конвейеру. */
static void raw_tcp_recved_cb(void *arg)
{
    LWIP_UNUSED_ARG(arg);
    g_recved_scheduled = 0;
    RawTcp_MaybeInjectStall();

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        tcp_client_t *c = &g_clients[id];
        uint32_t credit = ClientHandler_TakeRecvCredit(id);

        if (credit == 0U)    { continue; }
        if (c->pcb == NULL)  { continue; }   /* клиент уже ушёл -- подтверждать некому */

        /* Защита от рассинхрона учёта: подтверждать больше, чем принято,
         * нельзя (lwIP поймал бы это assert'ом на rcv_wnd > TCP_WND). */
        if (credit > c->rx_unacked) { credit = c->rx_unacked; }

        c->rx_unacked -= credit;
        c->last_activity_tick = osKernelGetTickCount();   /* приём идёт, просто медленно -- это активность */

        while (credit > 0U)
        {
            u16_t chunk = (credit > 0xFFFFU) ? (u16_t)0xFFFFU : (u16_t)credit;
            tcp_recved(c->pcb, chunk);
            credit -= chunk;
        }
    }
}

/*
 * Вызывается из ClientHandlerTask (каждую 1 мс, если есть что
 * подтверждать). Просит tcpip-поток вызвать tcp_recved() для всего
 * накопленного кредита. Если tcpip_callback не прошёл (mailbox полон) --
 * не страшно: кредит никуда не девается, следующий тик попробует снова.
 */
void RawTcpServer_KickRecved(void)
{
    if (g_recved_scheduled) { return; }
    g_recved_scheduled = 1;

    if (tcpip_callback(raw_tcp_recved_cb, NULL) != ERR_OK)
    {
        g_recved_scheduled = 0;
        g_recved_cb_fail++;
        if ((g_recved_cb_fail == 1U) || ((g_recved_cb_fail % 1000U) == 0U))
        {
            LOG_DBG("[TCP] WARNING: tcpip_callback(recved) failed, count=%lu\r\n",
                            (unsigned long)g_recved_cb_fail);
        }
    }
}

/*
 * Оставлена для совместимости: её вызывает CanTask, когда забирает кадр
 * из core_to_can_queue. Раньше она будила отложенный приём (rx_pending);
 * теперь приём ничего не удерживает, а подтверждение идёт из
 * ClientHandlerTask -- здесь делать нечего. Вызов из CanTask можно убрать.
 */
void RawTcpServer_NotifyChannelDrained(uint8_t channel_id)
{
    LWIP_UNUSED_ARG(channel_id);
}

/* ===== CALLBACKS ===== */

/* Когда закрылось ПОСЛЕДНЕЕ соединение (передача закончилась), печатаем
 * журнал ошибок, накопленный во время работы (event_log.c). */
/* Диагностика драйвера Ethernet (функции в ethernetif.c). Объявлены "weak":
 * если в ethernetif.c какой-то из них нет, ссылка будет NULL и вызов просто
 * пропустится -- сборка не сломается. */
extern void ETH_DebugPrintCounters(const char *tag) __attribute__((weak));
extern void ETH_PrintErrorBreakdown(void)          __attribute__((weak));

static void RawTcp_PrintEventLogIfIdle(void)
{
    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        if (g_clients[id].pcb != NULL) { return; }
    }
    if (ETH_DebugPrintCounters != NULL)  { ETH_DebugPrintCounters("end of transfer"); }
    if (ETH_PrintErrorBreakdown != NULL) { ETH_PrintErrorBreakdown(); }
    EventLog_PrintAndReset();
}

static void RawTcp_HandleClientGone(uint8_t id)
{
    g_clients[id].pcb = NULL;
    g_clients[id].rx_unacked = 0;
    RawTcp_TxReset(id);

    /* ДО ClientHandler_ClientDisconnected -- синтетическая команда
     * "C\r" должна уйти в очередь до того, как обнулятся буферы. */
    CoreTask_NotifyClientGone(id);
    ClientHandler_ClientDisconnected(id);
}

static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;
    LWIP_UNUSED_ARG(tpcb);
    LWIP_UNUSED_ARG(len);

    if (id >= MAX_CLIENTS) { return ERR_OK; }

    g_clients[id].last_activity_tick = osKernelGetTickCount();
    RawTcp_TryFlush(id);

    return ERR_OK;
}

static void tcp_server_error(void *arg, err_t err)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;

    if (id >= MAX_CLIENTS) { return; }

    LOG_INFO("[TCP] id=%u ch%u ERROR cb err=%d\r\n",
                    (unsigned)id, (unsigned)g_clients[id].channel_id, (int)err);
    RawTcp_PrintClientStats(id);
    LOG_INFO("==================== CONNECTION CLOSED (error) ====================\r\n\r\n");

    RawTcp_HandleClientGone(id);
    RawTcp_PrintEventLogIfIdle();
}

static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;

    if (id >= MAX_CLIENTS)
    {
        if (p != NULL) { pbuf_free(p); }
        return ERR_OK;
    }

    g_clients[id].last_activity_tick = osKernelGetTickCount();

    if (p == NULL)
    {
        LOG_INFO("[TCP] id=%u ch%u Client disconnected\r\n",
                        (unsigned)id, (unsigned)g_clients[id].channel_id);
        RawTcp_PrintClientStats(id);
        LOG_INFO("==================== CONNECTION CLOSED ====================\r\n\r\n");

        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_err(tpcb, NULL);

        err_t close_err = tcp_close(tpcb);
        if (close_err != ERR_OK) { tcp_abort(tpcb); }

        RawTcp_HandleClientGone(id);
        RawTcp_PrintEventLogIfIdle();
        return ERR_OK;
    }

    if (err != ERR_OK)
    {
        LOG_DBG("[TCP] id=%u RECV err=%d\r\n", (unsigned)id, (int)err);
        pbuf_free(p);
        return err;
    }

    /* Всё пришедшее -- сразу в RX-кольцо клиента, pbuf (DMA-буфер
     * Ethernet) -- сразу обратно в пул. tcp_recved() здесь НЕ зовём:
     * окно откроется, когда команды уйдут из кольца дальше
     * (raw_tcp_recved_cb). Место в кольце гарантировано: неподтверждённых
     * байт не больше TCP_WND, а кольцо >= 2*TCP_WND. */
    g_clients[id].rx_unacked += p->tot_len;
    if (g_clients[id].rx_unacked > g_clients[id].rx_unacked_max)
    {
        g_clients[id].rx_unacked_max = g_clients[id].rx_unacked;
    }

    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        ClientHandler_InputBytes(id, (const uint8_t *)q->payload, q->len);
    }
    pbuf_free(p);

    return ERR_OK;
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    /* arg здесь -- channel_id этого СЕРВЕРНОГО pcb (см. RawTcpServer_Init).
     * client_id назначается отдельно -- первый свободный слот в общем пуле. */
    uint8_t channel_id = (uint8_t)(uintptr_t)arg;
    uint8_t free_id = MAX_CLIENTS;

    if ((err != ERR_OK) || (newpcb == NULL))
    {
        LOG_INFO("[TCP] ch%u ACCEPT err=%d\r\n", (unsigned)channel_id, (int)err);
        return ERR_VAL;
    }

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        if (g_clients[id].pcb == NULL)
        {
            free_id = id;
            break;
        }
    }

    if (free_id == MAX_CLIENTS)
    {
        LOG_INFO("[TCP] ch%u Reject client -- all %u slots full (во всей системе)\r\n",
                        (unsigned)channel_id, (unsigned)MAX_CLIENTS);
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    g_clients[free_id].pcb = newpcb;
    g_clients[free_id].channel_id = channel_id;
    g_clients[free_id].last_activity_tick = osKernelGetTickCount();
    g_clients[free_id].tx_drop_count = 0;
    g_clients[free_id].tcp_err_mem_count = 0;
    g_clients[free_id].tcp_output_mem_count = 0;
    g_clients[free_id].stall_active = 0;
    g_clients[free_id].stall_count = 0;
    g_clients[free_id].stall_max_ms = 0;
    g_clients[free_id].stall_log_used = 0;
    g_clients[free_id].rx_unacked = 0;
    g_clients[free_id].rx_unacked_max = 0;
    RawTcp_TxReset(free_id);

    /* ClientHandler_ClientConnected обнуляет и кредит recv -- до того,
     * как на этот pcb придёт первый байт. */
    ClientHandler_ClientConnected(free_id, channel_id);
    CoreTask_ClientConnected(free_id, channel_id);

    LOG_INFO("\r\n==================== NEW CONNECTION ====================\r\n");
    LOG_INFO("[TCP] id=%u ch%u ACCEPT from %d.%d.%d.%d:%u (port %u)\r\n",
                    (unsigned)free_id, (unsigned)channel_id,
                    ip4_addr1(&newpcb->remote_ip),
                    ip4_addr2(&newpcb->remote_ip),
                    ip4_addr3(&newpcb->remote_ip),
                    ip4_addr4(&newpcb->remote_ip),
                    (unsigned)newpcb->remote_port,
                    (unsigned)TCP_SERVER_PORT[channel_id]);

    tcp_nagle_disable(newpcb);
    tcp_arg(newpcb, (void*)(uintptr_t)free_id);
    tcp_recv(newpcb, tcp_server_recv);
    tcp_err(newpcb, tcp_server_error);
    tcp_sent(newpcb, tcp_server_sent);

    return ERR_OK;
}

/* ===== PUBLIC API ===== */

int RawTcpServer_HasClient(uint8_t client_id)
{
    if (client_id >= MAX_CLIENTS) { return 0; }
    return (g_clients[client_id].pcb != NULL) ? 1 : 0;
}

int RawTcpServer_Send(uint8_t client_id, const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U))            { return -1; }
    if (client_id >= MAX_CLIENTS)                  { return -2; }
    if (g_clients[client_id].pcb == NULL)          { return -2; }
    if (RawTcp_TxPush(client_id, data, len) != 0)  { return -3; }
    RawTcp_TryFlush(client_id);
    return 0;
}

int RawTcpServer_SendAsync(uint8_t client_id, const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U))            { return -1; }
    if (client_id >= MAX_CLIENTS)                  { return -2; }
    if (g_clients[client_id].pcb == NULL)          { return -2; }

    if (RawTcp_TxPush(client_id, data, len) != 0)
    {
        /* Кольцо полно. Всё равно просим tcpip-поток попробовать его
         * протолкнуть: если прошлый tcp_write упал с ERR_MEM, а
         * неподтверждённых данных в полёте нет, sent-колбэк не придёт
         * никогда -- и без этого пинка кольцо осталось бы полным навсегда
         * (потерянное пробуждение на TX-стороне). ClientHandler повторяет
         * SendAsync каждую 1 мс, так что попытки идут регулярно. */
        RawTcp_ScheduleFlush(client_id);
        return -3;
    }

    RawTcp_ScheduleFlush(client_id);
    return 0;
}

void RawTcpServer_CheckIdleTimeout(void)
{
    uint32_t now = osKernelGetTickCount();

    for (uint8_t id = 0; id < MAX_CLIENTS; id++)
    {
        struct tcp_pcb *pcb = g_clients[id].pcb;

        if (pcb == NULL) { continue; }

        if ((now - g_clients[id].last_activity_tick) > CLIENT_IDLE_TIMEOUT_MS)
        {
            LOG_INFO("[TCP] id=%u ch%u idle timeout -- forcing abort\r\n",
                            (unsigned)id, (unsigned)g_clients[id].channel_id);
            RawTcp_PrintClientStats(id);
            LOG_INFO("==================== CONNECTION CLOSED (idle timeout) ====================\r\n\r\n");

            /* Отцепляем колбэки ДО tcp_abort: иначе tcp_abort вызовет
             * tcp_server_error, тот сделает HandleClientGone, и ниже мы
             * сделали бы его второй раз (двойной "C\r" и двойной лог). */
            tcp_arg(pcb, NULL);
            tcp_recv(pcb, NULL);
            tcp_sent(pcb, NULL);
            tcp_err(pcb, NULL);
            tcp_abort(pcb);

            RawTcp_HandleClientGone(id);
            RawTcp_PrintEventLogIfIdle();
        }
    }
}

/* ===== INIT ===== */

void RawTcpServer_Init(void)
{
    LOG_INFO("[TCP] Normal SLCAN mode -- multi-channel, NUM_CHANNELS=%u, MAX_CLIENTS=%u\r\n",
                    (unsigned)NUM_CHANNELS, (unsigned)MAX_CLIENTS);
    /* Проверка, что кольца действительно в DTCM (адрес должен быть 0x2000xxxx) */
    LOG_INFO("[TCP] TX rings: %u x %u bytes at 0x%08lX .. 0x%08lX\r\n",
             (unsigned)MAX_CLIENTS, (unsigned)RAW_TCP_TX_RING_SIZE_PER_CLIENT,
             (unsigned long)(uintptr_t)&g_tx_ring[0][0],
             (unsigned long)((uintptr_t)&g_tx_ring[0][0] + sizeof(g_tx_ring)));

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++)
    {
        if (g_server_pcb[ch] != NULL)
        {
            LOG_INFO("[TCP] ch%u previous server pcb exists\r\n", (unsigned)ch);
            continue;
        }

        struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (pcb == NULL)
        {
            LOG_ERR("[TCP] ch%u tcp_new_ip_type failed\r\n", (unsigned)ch);
            continue;
        }

        err_t err = tcp_bind(pcb, IP_ANY_TYPE, TCP_SERVER_PORT[ch]);
        if (err != ERR_OK)
        {
            LOG_ERR("[TCP] ch%u tcp_bind failed err=%d\r\n", (unsigned)ch, (int)err);
            tcp_close(pcb);
            continue;
        }

        err_t err2 = ERR_OK;
        /* backlog = MAX_CLIENTS -- с запасом, раз пул общий на оба канала */
        pcb = tcp_listen_with_backlog_and_err(pcb, MAX_CLIENTS, &err2);
        if (pcb == NULL)
        {
            LOG_ERR("[TCP] ch%u tcp_listen failed err=%d\r\n", (unsigned)ch, (int)err2);
            continue;
        }

        g_server_pcb[ch] = pcb;

        /* channel_id этого серверного pcb -- чтобы tcp_server_accept()
         * знал, на какой порт/канал пришло очередное подключение. */
        tcp_arg(pcb, (void*)(uintptr_t)ch);
        tcp_accept(pcb, tcp_server_accept);

        LOG_INFO("[TCP] ch%u Listening on port %u\r\n", (unsigned)ch, (unsigned)TCP_SERVER_PORT[ch]);
    }
}

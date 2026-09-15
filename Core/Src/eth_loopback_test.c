/*
 * eth_loopback_test.c
 */

#include "eth_loopback_test.h"
#include "main.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/etharp.h"   /* netif_is_link_up */
#include "cmsis_os.h"
#include "debug_uart.h"
#include <string.h>

extern struct netif gnetif;
extern ETH_HandleTypeDef heth;
extern void ETH_DebugPrintCounters(const char *tag);

/* Флаг активности теста — читается из ethernetif_input() (ethernetif.c),
 * чтобы понять, нужно ли перехватывать кадры с нашим EtherType до того,
 * как они уйдут в обычный lwIP/TCP-стек. Пока флаг = 0 (тест не запущен),
 * этот путь не тратит время ни на что, кроме одной проверки на кадр. */
volatile uint8_t g_eth_loopback_active = 0;

/* Наш опознавательный EtherType — из диапазона "Local Experimental
 * Ethertype", зарезервированного IEEE именно для таких целей: он
 * гарантированно не пересечётся с IP (0x0800) или ARP (0x0806), поэтому
 * lwIP его не тронет, если бы кадр всё же дошёл до обычного netif->input. */
#define ETH_LOOPBACK_ETHERTYPE   0x88B5U

#define ETH_LOOPBACK_FRAME_PAYLOAD   1024U   /* байт полезной нагрузки в кадре */
#define ETH_LOOPBACK_TARGET_BYTES    (1UL * 1024UL * 1024UL)    /* 1 МБ -- уменьшено
                                        для быстрой диагностики с паузами между
                                        кадрами; после подтверждения, что приём
                                        работает, можно вернуть 10 МБ */
#define ETH_LOOPBACK_PRINT_STEP      (128UL * 1024UL)           /* печать каждые 128 КБ */
#define ETH_LOOPBACK_INTER_FRAME_DELAY_MS   1U   /* пауза между кадрами --
                                        внутренний loopback-тракт PHY, судя по
                                        всему, не держит кадры "впритык" друг к
                                        другу без пауз (см. предыдущий прогон:
                                        10240/10240 кадров ушли без единой
                                        TX-ошибки, но вернулось RX-прерываниями
                                        только 2 -- похоже на переполнение
                                        внутреннего FIFO петли PHY) */

/* Счётчики, обновляемые из ethernetif_input() при приёме "своих" кадров */
static volatile uint32_t g_lb_rx_frames = 0;
static volatile uint32_t g_lb_rx_bytes  = 0;

/* Вызывается из ethernetif.c (ethernetif_input) для каждого принятого
 * кадра с EtherType == ETH_LOOPBACK_ETHERTYPE, пока g_eth_loopback_active
 * взведён. payload_len — длина полезной нагрузки без Ethernet-заголовка. */
void EthLoopback_OnFrameReceived(uint16_t payload_len)
{
    g_lb_rx_frames++;
    g_lb_rx_bytes += payload_len;
}

/* Прямая отправка сырого Ethernet-кадра через штатный TX-путь платы
 * (тот же low_level_output -> HAL_ETH_Transmit_IT, что и в обычной
 * работе) — реализована в ethernetif.c, объявлена там же как non-static
 * специально для этого теста. */
extern err_t EthLoopback_SendRawFrame(struct netif *netif,
                                      uint16_t ethertype,
                                      const uint8_t *payload,
                                      uint16_t payload_len);

/*
 * Обычно HAL_ETH_Start_IT() вызывается фоновой задачей ethernet_link_thread
 * (ethernetif.c) только когда PHY реально сообщает "линк поднят" -- то есть
 * только при подключённом кабеле. Для loopback-теста кабель намеренно не
 * нужен, поэтому этот механизм никогда не запустит MAC/DMA сам. Включение
 * loopback-бита в PHY не подделывает состояние линка автоматически -- PHY
 * по-прежнему может рапортовать "линк вниз", раз кабеля физически нет.
 * Поэтому здесь мы принудительно запускаем MAC/DMA сами, с фиксированными
 * параметрами (100M full duplex), если обычный механизм этого не сделал.
 */
static void EthLoopback_EnsureMacStarted(void)
{
    if (netif_is_link_up(&gnetif))
    {
        DebugUART_Print("[LOOPBACK] MAC/DMA уже запущен через обычное определение линка\r\n");
        return;
    }

    DebugUART_Print("[LOOPBACK] Реальный линк не обнаружен (ожидаемо без кабеля) -- "
                    "запускаю MAC/DMA принудительно для теста\r\n");

    ETH_MACConfigTypeDef MACConf = {0};
    HAL_ETH_GetMACConfig(&heth, &MACConf);
    MACConf.DuplexMode = ETH_FULLDUPLEX_MODE;
    MACConf.Speed      = ETH_SPEED_100M;
    HAL_ETH_SetMACConfig(&heth, &MACConf);

    if (HAL_ETH_Start_IT(&heth) != HAL_OK)
    {
        DebugUART_Print("[LOOPBACK] ERROR: HAL_ETH_Start_IT failed\r\n");
    }
    else
    {
        DebugUART_Print("[LOOPBACK] HAL_ETH_Start_IT OK (forced)\r\n");
    }
}

static int PHY_SetLoopback(uint8_t enable)
{
    uint32_t bmcr;

    if (!enable)
    {
        /* Возврат к обычному режиму: просто включаем автосогласование,
         * PHY сам заново согласует параметры с реальным линком (если
         * кабель подключат позже). */
        bmcr = (1UL << 12);   /* Auto-Negotiation Enable */

        if (HAL_ETH_WritePHYRegister(&heth, 0, 0 /* BMCR */, bmcr) != HAL_OK)
        {
            DebugUART_Print("[LOOPBACK] ERROR: write BMCR failed\r\n");
            return -1;
        }

        DebugUART_Print("[LOOPBACK] PHY loopback disabled (BMCR=0x%04lX)\r\n",
                        (unsigned long)bmcr);
        return 0;
    }

    /*
     * Программный сброс PHY перед настройкой loopback. Многие PHY
     * (в т.ч. по опыту типовых apnote для loopback-тестирования)
     * требуют сброса, чтобы новая конфигурация реально "защёлкнулась"
     * во внутренней логике, а не только в значении регистра BMCR --
     * иначе PHY может продолжать работать по старому внутреннему
     * состоянию (например, всё ещё ожидая согласования с линией),
     * даже если снаружи регистр уже показывает нужные биты.
     */
    if (HAL_ETH_WritePHYRegister(&heth, 0, 0 /* BMCR */, (1UL << 15) /* Reset */) != HAL_OK)
    {
        DebugUART_Print("[LOOPBACK] ERROR: soft reset write failed\r\n");
        return -1;
    }

    /* Ждём самоочищения бита Reset (PHY сигнализирует так о завершении
     * внутренней перезагрузки), с разумным таймаутом на случай, если
     * PHY почему-то не самоочистится вовремя. */
    {
        uint32_t reset_check = 0;
        uint32_t tries       = 0;
        const uint32_t max_tries = 50;   /* ~50 мс при HAL_Delay(1) на попытку */

        do
        {
            HAL_Delay(1);
            if (HAL_ETH_ReadPHYRegister(&heth, 0, 0 /* BMCR */, &reset_check) != HAL_OK)
            {
                DebugUART_Print("[LOOPBACK] ERROR: read BMCR during reset-wait failed\r\n");
                return -1;
            }
            tries++;
        } while (((reset_check & (1UL << 15)) != 0U) && (tries < max_tries));

        if ((reset_check & (1UL << 15)) != 0U)
        {
            DebugUART_Print("[LOOPBACK] WARNING: reset bit did not self-clear after %lu ms, "
                            "proceeding anyway\r\n", (unsigned long)tries);
        }
        else
        {
            DebugUART_Print("[LOOPBACK] PHY soft reset done (%lu ms)\r\n", (unsigned long)tries);
        }
    }

    /*
     * Для loopback пишем регистр СРАЗУ заданным значением, а не
     * добавляем бит Loopback поверх старого содержимого регистра.
     * Если оставить Auto-Negotiation включённым (как было после
     * обычной загрузки), PHY может остаться во внутреннем состоянии
     * "жду согласования с партнёром по линии" и не включить реальный
     * тракт данных петли, даже с выставленным битом Loopback --
     * особенно без подключённого кабеля. Поэтому здесь жёстко
     * фиксируем: Auto-Negotiation выключен, Speed=100M, Duplex=Full,
     * Loopback включён -- детерминированное состояние независимо от
     * того, подключён кабель или нет.
     */
    bmcr = (1UL << 14)   /* Loopback */
         | (1UL << 13)   /* Speed Select = 100 Mbps */
         | (1UL << 8);   /* Duplex Mode = Full */
         /* bit12 (Auto-Negotiation Enable) намеренно оставлен в 0 */

    if (HAL_ETH_WritePHYRegister(&heth, 0, 0 /* BMCR */, bmcr) != HAL_OK)
    {
        DebugUART_Print("[LOOPBACK] ERROR: write BMCR failed\r\n");
        return -1;
    }

    /* Читаем обратно для подтверждения, что регистр реально принял
     * записанное значение (а не просто "приняла шина MDIO", но PHY
     * внутренне что-то проигнорировал). */
    uint32_t readback = 0;
    if (HAL_ETH_ReadPHYRegister(&heth, 0, 0 /* BMCR */, &readback) != HAL_OK)
    {
        DebugUART_Print("[LOOPBACK] ERROR: read-back BMCR failed\r\n");
        return -1;
    }

    DebugUART_Print("[LOOPBACK] PHY loopback ENABLED, forced 100M/FULL, "
                    "AutoNeg OFF (BMCR written=0x%04lX, readback=0x%04lX)\r\n",
                    (unsigned long)bmcr, (unsigned long)readback);

    if (readback != bmcr)
    {
        DebugUART_Print("[LOOPBACK] WARNING: readback != written -- PHY may not have "
                        "accepted the exact value\r\n");
    }

    return 0;
}

static void EthLoopbackTest_Task(void *argument)
{
    (void)argument;

    uint8_t  payload[ETH_LOOPBACK_FRAME_PAYLOAD];
    uint32_t tx_bytes      = 0;
    uint32_t tx_frames     = 0;
    uint32_t last_print    = 0;
    uint32_t t_start;
    uint32_t t_end;
    uint32_t elapsed_ms;
    uint32_t speed_kbps;
    uint32_t consecutive_fail = 0;
    /* Защита от бесконечного вывода в UART: если подряд идёт слишком
     * много неудачных попыток отправки (например, MAC/DMA всё же не
     * запущен по другой причине), тест сам себя останавливает вместо
     * того чтобы затапливать лог одной и той же строкой без остановки. */
    #define ETH_LOOPBACK_MAX_CONSECUTIVE_FAIL   1000U

    for (uint32_t i = 0; i < sizeof(payload); i++)
    {
        payload[i] = (uint8_t)(i & 0xFFU);
    }

    /* Даём стеку/сети немного времени устаканиться перед стартом теста. */
    osDelay(500);

    DebugUART_Print("[LOOPBACK] === START === target=%lu bytes, frame_payload=%u\r\n",
                    (unsigned long)ETH_LOOPBACK_TARGET_BYTES,
                    (unsigned)ETH_LOOPBACK_FRAME_PAYLOAD);

    g_lb_rx_frames = 0;
    g_lb_rx_bytes  = 0;
    g_eth_loopback_active = 1;

    t_start = osKernelGetTickCount();

    while (tx_bytes < ETH_LOOPBACK_TARGET_BYTES)
    {
        /* Кладём номер кадра в первые 4 байта payload — пригодится,
         * если позже понадобится проверять целостность/порядок. */
        payload[0] = (uint8_t)(tx_frames >> 24);
        payload[1] = (uint8_t)(tx_frames >> 16);
        payload[2] = (uint8_t)(tx_frames >> 8);
        payload[3] = (uint8_t)(tx_frames);

        err_t st = EthLoopback_SendRawFrame(&gnetif,
                                            ETH_LOOPBACK_ETHERTYPE,
                                            payload,
                                            sizeof(payload));

        if (st != ERR_OK)
        {
            consecutive_fail++;
            if (consecutive_fail >= ETH_LOOPBACK_MAX_CONSECUTIVE_FAIL)
            {
                DebugUART_Print("[LOOPBACK] ERROR: %lu неудачных попыток подряд -- "
                                "останавливаю тест (MAC/DMA не запущен?)\r\n",
                                (unsigned long)consecutive_fail);
                g_eth_loopback_active = 0;
                return;
            }
            /* TX-пул временно занят (та же логика, что и в обычной
             * работе) — не страшно, просто пробуем ещё раз чуть позже,
             * без искусственных задержек: DMA освобождает слоты быстро. */
            continue;
        }

        consecutive_fail = 0;
        tx_frames++;
        tx_bytes += sizeof(payload);

        /* Пауза между кадрами -- дать loopback-тракту PHY время
         * "переварить" кадр перед следующим (см. комментарий у
         * ETH_LOOPBACK_INTER_FRAME_DELAY_MS выше). */
        osDelay(ETH_LOOPBACK_INTER_FRAME_DELAY_MS);

        if ((tx_bytes - last_print) >= ETH_LOOPBACK_PRINT_STEP)
        {
            last_print = tx_bytes;
            uint32_t now_ms = osKernelGetTickCount() - t_start;
            DebugUART_Print("[LOOPBACK] tx=%lu MB rx=%lu MB (%lu frames) | %lu ms\r\n",
                            (unsigned long)(tx_bytes / (1024UL * 1024UL)),
                            (unsigned long)(g_lb_rx_bytes / (1024UL * 1024UL)),
                            (unsigned long)g_lb_rx_frames,
                            (unsigned long)now_ms);
        }
    }

    /* Даём приёмному пути время дообработать кадры, которые ещё летят
     * "внутри" loopback (DMA + пара переключений задач). */
    osDelay(200);

    t_end      = osKernelGetTickCount();
    elapsed_ms = t_end - t_start;
    speed_kbps = (elapsed_ms > 0)
               ? (uint32_t)((uint64_t)g_lb_rx_bytes * 8ULL / elapsed_ms)
               : 0;

    DebugUART_Print("[LOOPBACK] === DONE ===\r\n");
    DebugUART_Print("[LOOPBACK] tx_frames=%lu tx_bytes=%lu\r\n",
                    (unsigned long)tx_frames, (unsigned long)tx_bytes);
    DebugUART_Print("[LOOPBACK] rx_frames=%lu rx_bytes=%lu\r\n",
                    (unsigned long)g_lb_rx_frames, (unsigned long)g_lb_rx_bytes);
    DebugUART_Print("[LOOPBACK] lost_frames=%ld\r\n",
                    (long)(tx_frames - g_lb_rx_frames));
    DebugUART_Print("[LOOPBACK] duration=%lu ms | speed=%lu Kbit/s (%lu Mbit/s)\r\n",
                    (unsigned long)elapsed_ms,
                    (unsigned long)speed_kbps,
                    (unsigned long)(speed_kbps / 1000UL));

    /* Если rx_frames=0, эта строка сразу покажет, дошло ли вообще хоть
     * одно RX-прерывание до платы (rx_irq/rx_sem) -- если счётчики
     * ненулевые, кадры физически приходят, но что-то не так дальше по
     * цепочке (например, наш хук в ethernetif_input); если нулевые --
     * проблема на уровне PHY/петли, кадры не возвращаются вообще. */
    ETH_DebugPrintCounters("LOOPBACK-DONE");

    /* Сырое состояние регистра статуса DMA-канала -- показывает флаги
     * низкого уровня (в т.ч. был ли вообще зафиксирован приём/ошибка
     * приёма на аппаратном уровне), независимо от того, сработал ли
     * наш программный колбэк. Регистр DMACSR уже используется в
     * проекте (см. ETH_DMACSR_RBU в HAL_ETH_ErrorCallback), поэтому
     * его чтение здесь безопасно и не требует новых предположений. */
    DebugUART_Print("[LOOPBACK] raw ETH->DMACSR=0x%08lX ETH->DMAMR=0x%08lX\r\n",
                    (unsigned long)heth.Instance->DMACSR,
                    (unsigned long)heth.Instance->DMAMR);

    g_eth_loopback_active = 0;

    for (;;)
    {
        osDelay(10000);
    }
}

void EthLoopbackTest_Start(void)
{
    if (PHY_SetLoopback(1) != 0)
    {
        DebugUART_Print("[LOOPBACK] ERROR: could not enable PHY loopback, aborting test\r\n");
        return;
    }

    /* Даём фоновой задаче ethernet_link_thread шанс самой поднять MAC/DMA,
     * если кабель всё же подключён -- и подстраховываемся принудительным
     * запуском, если нет (см. комментарий у EthLoopback_EnsureMacStarted). */
    osDelay(300);
    EthLoopback_EnsureMacStarted();

    const osThreadAttr_t attr = {
        .name       = "EthLoopback",
        .stack_size = 4096,
        .priority   = (osPriority_t)osPriorityNormal,
    };

    osThreadId_t h = osThreadNew(EthLoopbackTest_Task, NULL, &attr);

    if (h == NULL)
    {
        DebugUART_Print("[LOOPBACK] ERROR: osThreadNew failed\r\n");
    }
}

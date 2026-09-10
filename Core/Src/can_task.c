/*
 * can_task.c
 *
 *  Created on: Mar 10, 2026
 *      Author: Egenie
 *
 *  Убрана диагностическая инструментация замера времени (CAN-TIMING2),
 *  которая использовалась только для поиска причины низкой скорости
 *  CanTask -- больше не нужна для обычной работы, только засоряла лог
 *  и сама по себе немного нагружала систему лишней печатью.
 */

#include "can_task.h"
#include "cmsis_os.h"
#include "app_queues.h"
#include "debug_uart.h"
#include "can_types.h"
#include "main.h"

#include <string.h>
#include <stdint.h>

static osThreadId_t canTaskHandle = NULL;
extern FDCAN_HandleTypeDef hfdcan1;

static uint8_t g_can_started = 0;
static volatile uint32_t g_can_rx_irq_count = 0;
static volatile uint32_t g_can_rx_ok_count = 0;
static volatile uint32_t g_can_rx_queue_drop_count = 0;

typedef struct
{
    uint32_t prescaler;
    uint32_t sjw;
    uint32_t tseg1;
    uint32_t tseg2;
} can_bittiming_t;

static int CanTask_BuildTxHeader(const can_frame_t *frame, FDCAN_TxHeaderTypeDef *hdr);
static int CanTask_GetBitTiming(uint32_t bitrate_bps, can_bittiming_t *bt);
static int CanTask_ApplyBitrate(uint32_t bitrate_bps);
static int CanTask_FdcanRxToCanFrame(const FDCAN_RxHeaderTypeDef *rx_hdr,
                                     const uint8_t *rx_data,
                                     can_frame_t *out_frame);
static void CanTask(void *argument);
static void CanTask_PrintTxState(const char *tag);

static int CanTask_BuildTxHeader(const can_frame_t *frame, FDCAN_TxHeaderTypeDef *hdr)
{
    if ((frame == NULL) || (hdr == NULL))
    {
        return -1;
    }

    if (frame->Size > 8U)
    {
        return -1;
    }

    memset(hdr, 0, sizeof(*hdr));

    hdr->Identifier = frame->Id;

    if ((frame->Flags & CAN_FLAG_EXTENDED) != 0U)
    {
        hdr->IdType = FDCAN_EXTENDED_ID;
    }
    else
    {
        if (frame->Id > 0x7FFU)
        {
            return -1;
        }

        hdr->IdType = FDCAN_STANDARD_ID;
    }

    hdr->TxFrameType = FDCAN_DATA_FRAME;

    switch (frame->Size)
    {
        case 0: hdr->DataLength = FDCAN_DLC_BYTES_0; break;
        case 1: hdr->DataLength = FDCAN_DLC_BYTES_1; break;
        case 2: hdr->DataLength = FDCAN_DLC_BYTES_2; break;
        case 3: hdr->DataLength = FDCAN_DLC_BYTES_3; break;
        case 4: hdr->DataLength = FDCAN_DLC_BYTES_4; break;
        case 5: hdr->DataLength = FDCAN_DLC_BYTES_5; break;
        case 6: hdr->DataLength = FDCAN_DLC_BYTES_6; break;
        case 7: hdr->DataLength = FDCAN_DLC_BYTES_7; break;
        case 8: hdr->DataLength = FDCAN_DLC_BYTES_8; break;
        default:
            return -1;
    }

    /*
     * Поля TxHeader сделаны как в прошивке Славы.
     */
    hdr->ErrorStateIndicator = FDCAN_ESI_PASSIVE;
    hdr->BitRateSwitch = FDCAN_BRS_OFF;
    hdr->FDFormat = FDCAN_CLASSIC_CAN;
    hdr->TxEventFifoControl = FDCAN_STORE_TX_EVENTS;
    hdr->MessageMarker = 0xDD;

    return 0;
}

static int CanTask_GetBitTiming(uint32_t bitrate_bps, can_bittiming_t *bt)
{
    if (bt == NULL)
    {
        return -1;
    }

    memset(bt, 0, sizeof(*bt));

    switch (bitrate_bps)
    {
        case 500000U:
            bt->prescaler = 1;
            bt->sjw       = 13;
            bt->tseg1     = 86;
            bt->tseg2     = 13;
            return 0;

        case 1000000U:
            bt->prescaler = 1;
            bt->sjw       = 1;
            bt->tseg1     = 48;
            bt->tseg2     = 1;
            return 0;

        default:
            return -1;
    }
}

static int CanTask_ApplyBitrate(uint32_t bitrate_bps)
{
    can_bittiming_t bt;

    if (CanTask_GetBitTiming(bitrate_bps, &bt) != 0)
    {
        DebugUART_Print("[CAN] ERROR: unsupported bitrate %lu bit/s\r\n",
                        (unsigned long)bitrate_bps);
        return -1;
    }

    hfdcan1.Init.NominalPrescaler     = bt.prescaler;
    hfdcan1.Init.NominalSyncJumpWidth = bt.sjw;
    hfdcan1.Init.NominalTimeSeg1      = bt.tseg1;
    hfdcan1.Init.NominalTimeSeg2      = bt.tseg2;

    DebugUART_Print("[CAN] bitrate applied: %lu bit/s -> Presc=%lu SJW=%lu TSEG1=%lu TSEG2=%lu\r\n",
                    (unsigned long)bitrate_bps,
                    (unsigned long)bt.prescaler,
                    (unsigned long)bt.sjw,
                    (unsigned long)bt.tseg1,
                    (unsigned long)bt.tseg2);

    return 0;
}

int CanTask_Open(core_can_mode_t mode, uint32_t bitrate_bps)
{
    FDCAN_FilterTypeDef sFilterConfig;
    can_bittiming_t bt;

    if (g_can_started)
    {
        if (HAL_FDCAN_Stop(&hfdcan1) != HAL_OK)
        {
            DebugUART_Print("[CAN] ERROR: HAL_FDCAN_Stop failed before reopen\r\n");
            return -1;
        }

        g_can_started = 0;
        DebugUART_Print("[CAN] controller stopped before reopen\r\n");
    }

    if (CanTask_GetBitTiming(bitrate_bps, &bt) != 0)
    {
        DebugUART_Print("[CAN] ERROR: unsupported bitrate %lu bit/s\r\n",
                        (unsigned long)bitrate_bps);
        return -1;
    }

    hfdcan1.Init.NominalPrescaler     = bt.prescaler;
    hfdcan1.Init.NominalSyncJumpWidth = bt.sjw;
    hfdcan1.Init.NominalTimeSeg1      = bt.tseg1;
    hfdcan1.Init.NominalTimeSeg2      = bt.tseg2;

    if (bitrate_bps == 500000U)
    {
        hfdcan1.Init.DataPrescaler = 25;
        hfdcan1.Init.DataSyncJumpWidth = 1;
        hfdcan1.Init.DataTimeSeg1 = 2;
        hfdcan1.Init.DataTimeSeg2 = 1;
    }
    else
    {
        hfdcan1.Init.DataPrescaler = 13;
        hfdcan1.Init.DataSyncJumpWidth = 1;
        hfdcan1.Init.DataTimeSeg1 = 2;
        hfdcan1.Init.DataTimeSeg2 = 1;
    }

    switch (mode)
    {
        case CORE_CAN_MODE_NORMAL:
            hfdcan1.Init.Mode = FDCAN_MODE_NORMAL;
            break;

        case CORE_CAN_MODE_LISTEN_ONLY:
            hfdcan1.Init.Mode = FDCAN_MODE_BUS_MONITORING;
            break;

        case CORE_CAN_MODE_SELF_RECEPTION:
            hfdcan1.Init.Mode = FDCAN_MODE_INTERNAL_LOOPBACK;
            break;

        default:
            DebugUART_Print("[CAN] ERROR: invalid mode in CanTask_Open\r\n");
            return -1;
    }

    if (HAL_FDCAN_Init(&hfdcan1) != HAL_OK)
    {
        DebugUART_Print("[CAN] ERROR: HAL_FDCAN_Init failed in CanTask_Open\r\n");
        return -1;
    }

    memset(&sFilterConfig, 0, sizeof(sFilterConfig));

    sFilterConfig.IdType = FDCAN_STANDARD_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x000;
    sFilterConfig.FilterID2 = 0x000;

    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilterConfig) != HAL_OK)
    {
        DebugUART_Print("[CAN] ERROR: HAL_FDCAN_ConfigFilter failed in CanTask_Open\r\n");
        return -1;
    }

    sFilterConfig.IdType = FDCAN_EXTENDED_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x00000000;
    sFilterConfig.FilterID2 = 0x00000000;

    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilterConfig) != HAL_OK)
    {
        DebugUART_Print("[CAN] ERROR: HAL_FDCAN_ConfigFilter EXT failed in CanTask_Open\r\n");
        return -1;
    }

    if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1,
                                     FDCAN_ACCEPT_IN_RX_FIFO0,
                                     FDCAN_ACCEPT_IN_RX_FIFO0,
                                     FDCAN_REJECT_REMOTE,
                                     FDCAN_REJECT_REMOTE) != HAL_OK)
    {
        DebugUART_Print("[CAN] ERROR: HAL_FDCAN_ConfigGlobalFilter failed in CanTask_Open\r\n");
        return -1;
    }

    if (HAL_FDCAN_ConfigInterruptLines(&hfdcan1,
                                       FDCAN_IT_RX_FIFO0_NEW_MESSAGE,
                                       FDCAN_INTERRUPT_LINE0) != HAL_OK)
    {
        DebugUART_Print("[CAN] ERROR: HAL_FDCAN_ConfigInterruptLines failed in CanTask_Open\r\n");
        return -1;
    }

    if (HAL_FDCAN_ActivateNotification(&hfdcan1,
                                       FDCAN_IT_RX_FIFO0_NEW_MESSAGE,
                                       0) != HAL_OK)
    {
        DebugUART_Print("[CAN] ERROR: HAL_FDCAN_ActivateNotification failed in CanTask_Open\r\n");
        return -1;
    }

    if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK)
    {
        DebugUART_Print("[CAN] ERROR: HAL_FDCAN_Start failed in CanTask_Open\r\n");
        return -1;
    }

    g_can_started = 1;
    g_can_rx_irq_count = 0;
    g_can_rx_ok_count = 0;
    g_can_rx_queue_drop_count = 0;

    DebugUART_Print("[CAN] RX notification active\r\n");
    DebugUART_Print("[CAN] channel opened, mode=%lu bitrate=%lu bit/s\r\n",
                    (unsigned long)hfdcan1.Init.Mode,
                    (unsigned long)bitrate_bps);

    return 0;
}

int CanTask_Close(void)
{
    if (!g_can_started)
    {
        DebugUART_Print("[CAN] close requested, controller already stopped\r\n");
        return 0;
    }

    /*
     * Диагностика: сколько раз реально сработало прерывание приёма
     * (g_can_rx_irq_count) и сколько кадров из них успешно разобрано и
     * поставлено в can_to_core_queue (g_can_rx_ok_count) за время,
     * пока канал был открыт. Печатаем здесь, а не в самом callback,
     * потому что это настоящее прерывание -- вызывать DebugUART_Print
     * оттуда нельзя (мьютекс). CanTask_Close() вызывается из потока
     * CoreTask, печатать тут безопасно.
     */
    DebugUART_Print("[CAN] RX stats for this session: irq=%lu ok=%lu queue_drops=%lu\r\n",
                    (unsigned long)g_can_rx_irq_count,
                    (unsigned long)g_can_rx_ok_count,
                    (unsigned long)g_can_rx_queue_drop_count);

    if (HAL_FDCAN_Stop(&hfdcan1) != HAL_OK)
    {
        DebugUART_Print("[CAN] ERROR: HAL_FDCAN_Stop failed on close\r\n");
        return -1;
    }

    g_can_started = 0;

    DebugUART_Print("[CAN] channel stopped\r\n");
    return 0;
}

static void CanTask(void *argument)
{
    (void)argument;

    can_msg_t can_msg;
    FDCAN_TxHeaderTypeDef tx_hdr;
    uint8_t tx_data[8];

    static uint32_t tx_fifo_full_count = 0;
    static uint32_t add_message_fail_count = 0;

    DebugUART_Print("[CAN] CanTask started\r\n");
    DebugUART_Print("[CAN] core_to_can_queue=%p can_to_core_queue=%p\r\n",
                    (void*)core_to_can_queue,
                    (void*)can_to_core_queue);

    for (;;)
    {
        if (osMessageQueueGet(core_to_can_queue, &can_msg, NULL, osWaitForever) == osOK)
        {
            memset(&tx_hdr, 0, sizeof(tx_hdr));
            memset(tx_data, 0, sizeof(tx_data));

            if (CanTask_BuildTxHeader(&can_msg.frame, &tx_hdr) != 0)
            {
                DebugUART_Print("[CAN] ERROR: failed to build FDCAN TX header\r\n");
                continue;
            }

            memcpy(tx_data, can_msg.frame.Data, can_msg.frame.Size);

            if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) == 0U)
            {
                tx_fifo_full_count++;
                if ((tx_fifo_full_count % 200U) == 0U)
                {
                    DebugUART_Print("[CAN] ERROR: TX FIFO FULL, frame skipped "
                                    "(count=%lu) TXFQS=0x%08lX PSR=0x%08lX\r\n",
                                    (unsigned long)tx_fifo_full_count,
                                    (unsigned long)hfdcan1.Instance->TXFQS,
                                    (unsigned long)hfdcan1.Instance->PSR);
                }
                continue;
            }

            if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &tx_hdr, tx_data) != HAL_OK)
            {
                add_message_fail_count++;
                if ((add_message_fail_count % 200U) == 0U)
                {
                    uint32_t err = HAL_FDCAN_GetError(&hfdcan1);
                    DebugUART_Print("[CAN] ERROR: AddMessageToTxFifoQ failed "
                                    "(count=%lu) err=0x%08lX TXFQS=0x%08lX\r\n",
                                    (unsigned long)add_message_fail_count,
                                    (unsigned long)err,
                                    (unsigned long)hfdcan1.Instance->TXFQS);
                }
                continue;
            }

            /* Кадр успешно добавлен в TX FIFO -- сразу переходим к следующему. */
        }
    }
}

void CanTask_Start(void)
{
    const osThreadAttr_t attr = {
        .name = "CanTask",
        .stack_size = 4096,
        .priority = (osPriority_t)osPriorityAboveNormal
    };

    canTaskHandle = osThreadNew(CanTask, NULL, &attr);

    if (!canTaskHandle)
    {
        DebugUART_Print("[CAN] ERROR: task create failed\r\n");
    }
    else
    {
        DebugUART_Print("[CAN] task created\r\n");
    }
}

static int CanTask_FdcanRxToCanFrame(const FDCAN_RxHeaderTypeDef *rx_hdr,
                                     const uint8_t *rx_data,
                                     can_frame_t *out_frame)
{
    if ((rx_hdr == NULL) || (out_frame == NULL))
    {
        return -1;
    }

    memset(out_frame, 0, sizeof(*out_frame));

    out_frame->Id = rx_hdr->Identifier;
    out_frame->Timestamp = 0U;

    if (rx_hdr->IdType == FDCAN_EXTENDED_ID)
    {
        out_frame->Flags |= CAN_FLAG_EXTENDED;
    }

    if (rx_hdr->RxFrameType == FDCAN_REMOTE_FRAME)
    {
        out_frame->Flags |= CAN_FLAG_RTR;
    }

    switch (rx_hdr->DataLength)
    {
        case FDCAN_DLC_BYTES_0: out_frame->Size = 0; break;
        case FDCAN_DLC_BYTES_1: out_frame->Size = 1; break;
        case FDCAN_DLC_BYTES_2: out_frame->Size = 2; break;
        case FDCAN_DLC_BYTES_3: out_frame->Size = 3; break;
        case FDCAN_DLC_BYTES_4: out_frame->Size = 4; break;
        case FDCAN_DLC_BYTES_5: out_frame->Size = 5; break;
        case FDCAN_DLC_BYTES_6: out_frame->Size = 6; break;
        case FDCAN_DLC_BYTES_7: out_frame->Size = 7; break;
        case FDCAN_DLC_BYTES_8: out_frame->Size = 8; break;
        default:
            return -1;
    }

    if (((out_frame->Flags & CAN_FLAG_RTR) == 0U) && (rx_data != NULL))
    {
        memcpy(out_frame->Data, rx_data, out_frame->Size);
    }

    return 0;
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    FDCAN_RxHeaderTypeDef rx_hdr;
    uint8_t rx_data[8];
    can_msg_t can_msg;

    if (hfdcan == NULL)
    {
        return;
    }

    if (hfdcan->Instance != FDCAN1)
    {
        return;
    }

    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) == 0U)
    {
        return;
    }

    g_can_rx_irq_count++;

    memset(&rx_hdr, 0, sizeof(rx_hdr));
    memset(rx_data, 0, sizeof(rx_data));
    memset(&can_msg, 0, sizeof(can_msg));

    if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &rx_hdr, rx_data) != HAL_OK)
    {
        return;
    }

    if (CanTask_FdcanRxToCanFrame(&rx_hdr, rx_data, &can_msg.frame) != 0)
    {
        return;
    }

    if (can_to_core_queue == NULL)
    {
        return;
    }

    g_can_rx_ok_count++;

    /*
     * В callback нельзя вызывать DebugUART_Print (мьютекс) -- это
     * настоящее прерывание. Кладём без ожидания, счётчик потерь
     * доступен для диагностики без печати из ISR.
     */
    if (osMessageQueuePut(can_to_core_queue, &can_msg, 0, 0) != osOK)
    {
        g_can_rx_queue_drop_count++;
    }
}

static void CanTask_PrintTxState(const char *tag)
{
    DebugUART_Print("[CAN TX] %s TXFQS=0x%08lX TXBRP=0x%08lX TXBTO=0x%08lX TXBCF=0x%08lX PSR=0x%08lX ECR=0x%08lX\r\n",
                    tag,
                    (unsigned long)hfdcan1.Instance->TXFQS,
                    (unsigned long)hfdcan1.Instance->TXBRP,
                    (unsigned long)hfdcan1.Instance->TXBTO,
                    (unsigned long)hfdcan1.Instance->TXBCF,
                    (unsigned long)hfdcan1.Instance->PSR,
                    (unsigned long)hfdcan1.Instance->ECR);
}

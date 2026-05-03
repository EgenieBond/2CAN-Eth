/*
 * can_task.c
 *
 *  Created on: Mar 10, 2026
 *      Author: Egenie
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

    hdr->TxFrameType = ((frame->Flags & CAN_FLAG_RTR) != 0U)
                     ? FDCAN_REMOTE_FRAME
                     : FDCAN_DATA_FRAME;

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

    hdr->ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    hdr->BitRateSwitch = FDCAN_BRS_OFF;
    hdr->FDFormat = FDCAN_CLASSIC_CAN;
    hdr->TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    hdr->MessageMarker = 0;

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
        case 10000U:
            bt->prescaler = 125;
            bt->sjw       = 1;
            bt->tseg1     = 12;
            bt->tseg2     = 7;
            return 0;

        case 20000U:
            bt->prescaler = 125;
            bt->sjw       = 1;
            bt->tseg1     = 7;
            bt->tseg2     = 2;
            return 0;

        case 50000U:
            bt->prescaler = 25;
            bt->sjw       = 1;
            bt->tseg1     = 12;
            bt->tseg2     = 7;
            return 0;

        case 100000U:
            bt->prescaler = 25;
            bt->sjw       = 1;
            bt->tseg1     = 7;
            bt->tseg2     = 2;
            return 0;

        case 125000U:
            bt->prescaler = 10;
            bt->sjw       = 1;
            bt->tseg1     = 12;
            bt->tseg2     = 7;
            return 0;

        case 250000U:
            bt->prescaler = 5;
            bt->sjw       = 1;
            bt->tseg1     = 12;
            bt->tseg2     = 7;
            return 0;

        case 500000U:
            bt->prescaler = 2;
            bt->sjw       = 1;
            bt->tseg1     = 16;
            bt->tseg2     = 8;
            return 0;

        case 800000U:
            bt->prescaler = 1;
            bt->sjw       = 1;
            bt->tseg1     = 22;
            bt->tseg2     = 8;
            return 0;

        case 1000000U:
            bt->prescaler = 1;
            bt->sjw       = 1;
            bt->tseg1     = 16;
            bt->tseg2     = 8;
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
    FDCAN_FilterTypeDef sFilter;

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

    if (CanTask_ApplyBitrate(bitrate_bps) != 0)
    {
        return -1;
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

    memset(&sFilter, 0, sizeof(sFilter));

    sFilter.IdType       = FDCAN_STANDARD_ID;
    sFilter.FilterIndex  = 0;
    sFilter.FilterType   = FDCAN_FILTER_MASK;
    sFilter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilter.FilterID1    = 0x000;
    sFilter.FilterID2    = 0x000;

    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilter) != HAL_OK)
    {
        DebugUART_Print("[CAN] ERROR: HAL_FDCAN_ConfigFilter failed in CanTask_Open\r\n");
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

    DebugUART_Print("[CAN] channel opened, mode=%lu bitrate=%lu bit/s\r\n",
                    (unsigned long)hfdcan1.Init.Mode,
                    (unsigned long)bitrate_bps);
    DebugUART_Print("[CAN] NBTP=0x%08lX\r\n", (unsigned long)hfdcan1.Instance->NBTP);
    DebugUART_Print("[CAN] PSR=0x%08lX\r\n", (unsigned long)hfdcan1.Instance->PSR);
    DebugUART_Print("[CAN] CCCR=0x%08lX\r\n", (unsigned long)hfdcan1.Instance->CCCR);

    return 0;
}

int CanTask_Close(void)
{
    if (!g_can_started)
    {
        DebugUART_Print("[CAN] close requested, controller already stopped\r\n");
        return 0;
    }

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

            /*
             * В нагрузочном режиме не печатаем каждый кадр в UART,
             * иначе UART сильно тормозит систему и забивает TCP.
             */

            if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) == 0U)
            {
                DebugUART_Print("[CAN] ERROR: TX FIFO FULL, frame skipped\r\n");
                DebugUART_Print("[CAN] TXFQS=0x%08lX PSR=0x%08lX CCCR=0x%08lX\r\n",
                                (unsigned long)hfdcan1.Instance->TXFQS,
                                (unsigned long)hfdcan1.Instance->PSR,
                                (unsigned long)hfdcan1.Instance->CCCR);
                continue;
            }

            if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &tx_hdr, tx_data) != HAL_OK)
            {
                uint32_t err = HAL_FDCAN_GetError(&hfdcan1);
                DebugUART_Print("[CAN] ERROR: HAL_FDCAN_AddMessageToTxFifoQ failed, err=0x%08lX\r\n",
                                (unsigned long)err);
                DebugUART_Print("[CAN] TXFQS=0x%08lX PSR=0x%08lX CCCR=0x%08lX\r\n",
                                (unsigned long)hfdcan1.Instance->TXFQS,
                                (unsigned long)hfdcan1.Instance->PSR,
                                (unsigned long)hfdcan1.Instance->CCCR);
                continue;
            }
        }
    }
}

void CanTask_Start(void)
{
    const osThreadAttr_t attr = {
        .name = "CanTask",
        .stack_size = 4096,
        .priority = (osPriority_t)osPriorityNormal
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

    /*
     * В callback нельзя ждать, поэтому кладём без ожидания.
     * Если очередь переполнена — кадр теряется, но система не зависает.
     */
    (void)osMessageQueuePut(can_to_core_queue, &can_msg, 0, 0);
}

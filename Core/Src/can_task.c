/*
 * can_task.c
 *
 *  Created on: Mar 10, 2026
 *      Author: Egenie
 *
 *  Обновлено: поддержка ДВУХ независимых физических CAN-каналов
 *  (FDCAN1/FDCAN2). Вместо единственного набора глобальных переменных
 *  -- массив контекстов g_channels[NUM_CHANNELS], по одному на канал.
 *  Задача CanTask() теперь параметризована channel_id (передаётся как
 *  аргумент потока) -- на каждый канал запускается своя копия задачи,
 *  каждая блокируется на СВОЕЙ паре очередей (core_to_can_queues[N]/
 *  can_to_core_queues[N]).
 *
 *  HAL_FDCAN_RxFifo0Callback() остаётся ОДНОЙ функцией на оба канала --
 *  HAL сам передаёт нужный хендл (&hfdcan1 или &hfdcan2), по нему
 *  определяем channel_id и кладём кадр в очередь нужного канала.
 */

#include "can_task.h"
#include "cmsis_os.h"
#include "app_queues.h"
#include "debug_uart.h"
#include "can_types.h"
#include "main.h"

#include <string.h>
#include <stdint.h>

extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan2;

typedef struct
{
    uint32_t prescaler;
    uint32_t sjw;
    uint32_t tseg1;
    uint32_t tseg2;
} can_bittiming_t;

typedef struct
{
    FDCAN_HandleTypeDef *hfdcan;
    osThreadId_t          task_handle;
    uint8_t               started;

    volatile uint32_t rx_irq_count;
    volatile uint32_t rx_ok_count;
    volatile uint32_t rx_queue_drop_count;
} can_channel_ctx_t;

static can_channel_ctx_t g_channels[NUM_CHANNELS] =
{
    { NULL, NULL, 0, 0, 0, 0 },   /* канал 0 -- заполнится &hfdcan1 в CanTask_Start */
    { NULL, NULL, 0, 0, 0, 0 },   /* канал 1 -- заполнится &hfdcan2 в CanTask_Start */
};

static int CanTask_BuildTxHeader(const can_frame_t *frame, FDCAN_TxHeaderTypeDef *hdr);
static int CanTask_GetBitTiming(uint32_t bitrate_bps, can_bittiming_t *bt);
static int CanTask_FdcanRxToCanFrame(const FDCAN_RxHeaderTypeDef *rx_hdr,
                                     const uint8_t *rx_data,
                                     can_frame_t *out_frame);
static void CanTask(void *argument);

static FDCAN_HandleTypeDef *ChannelHandle(uint8_t channel_id)
{
    if (channel_id >= NUM_CHANNELS) { return NULL; }
    return g_channels[channel_id].hfdcan;
}

static int8_t ChannelIdFromHandle(const FDCAN_HandleTypeDef *hfdcan)
{
    for (uint8_t i = 0; i < NUM_CHANNELS; i++)
    {
        if (g_channels[i].hfdcan == hfdcan) { return (int8_t)i; }
    }
    return -1;
}

static int CanTask_BuildTxHeader(const can_frame_t *frame, FDCAN_TxHeaderTypeDef *hdr)
{
    if ((frame == NULL) || (hdr == NULL)) { return -1; }
    if (frame->Size > 8U) { return -1; }

    memset(hdr, 0, sizeof(*hdr));
    hdr->Identifier = frame->Id;

    if ((frame->Flags & CAN_FLAG_EXTENDED) != 0U)
    {
        hdr->IdType = FDCAN_EXTENDED_ID;
    }
    else
    {
        if (frame->Id > 0x7FFU) { return -1; }
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
        default: return -1;
    }

    hdr->ErrorStateIndicator = FDCAN_ESI_PASSIVE;
    hdr->BitRateSwitch = FDCAN_BRS_OFF;
    hdr->FDFormat = FDCAN_CLASSIC_CAN;
    hdr->TxEventFifoControl = FDCAN_STORE_TX_EVENTS;
    hdr->MessageMarker = 0xDD;

    return 0;
}

static int CanTask_GetBitTiming(uint32_t bitrate_bps, can_bittiming_t *bt)
{
    if (bt == NULL) { return -1; }
    memset(bt, 0, sizeof(*bt));

    switch (bitrate_bps)
    {
        case 500000U:
            bt->prescaler = 1; bt->sjw = 13; bt->tseg1 = 86; bt->tseg2 = 13;
            return 0;
        case 800000U:
            /* Точное 800 кбит/с недостижимо при fdcan_ker_ck=50 МГц (нужно
             * 62.5 кванта -- дробное число). Ближайшее целое -- 63 кванта,
             * реальная скорость 50000000/63 = 793650 бит/с (-0.79% от
             * номинала) -- в пределах обычного допуска CAN. */
            bt->prescaler = 1; bt->sjw = 8; bt->tseg1 = 54; bt->tseg2 = 8;
            return 0;
        case 1000000U:
            bt->prescaler = 1; bt->sjw = 1; bt->tseg1 = 48; bt->tseg2 = 1;
            return 0;
        default:
            return -1;
    }
}

int CanTask_Open(uint8_t channel_id, core_can_mode_t mode, uint32_t bitrate_bps)
{
    FDCAN_FilterTypeDef sFilterConfig;
    can_bittiming_t bt;
    FDCAN_HandleTypeDef *hfdcan = ChannelHandle(channel_id);

    if (hfdcan == NULL)
    {
        DebugUART_Print("[CAN] id=%u ERROR: invalid channel_id\r\n", (unsigned)channel_id);
        return -1;
    }

    if (g_channels[channel_id].started)
    {
        if (HAL_FDCAN_Stop(hfdcan) != HAL_OK)
        {
            DebugUART_Print("[CAN] ch%u ERROR: HAL_FDCAN_Stop failed before reopen\r\n",
                            (unsigned)channel_id);
            return -1;
        }
        g_channels[channel_id].started = 0;
        DebugUART_Print("[CAN] ch%u controller stopped before reopen\r\n", (unsigned)channel_id);
    }

    if (CanTask_GetBitTiming(bitrate_bps, &bt) != 0)
    {
        DebugUART_Print("[CAN] ch%u ERROR: unsupported bitrate %lu bit/s\r\n",
                        (unsigned)channel_id, (unsigned long)bitrate_bps);
        return -1;
    }

    hfdcan->Init.NominalPrescaler     = bt.prescaler;
    hfdcan->Init.NominalSyncJumpWidth = bt.sjw;
    hfdcan->Init.NominalTimeSeg1      = bt.tseg1;
    hfdcan->Init.NominalTimeSeg2      = bt.tseg2;

    if (bitrate_bps == 500000U)
    {
        hfdcan->Init.DataPrescaler = 25;
        hfdcan->Init.DataSyncJumpWidth = 1;
        hfdcan->Init.DataTimeSeg1 = 2;
        hfdcan->Init.DataTimeSeg2 = 1;
    }
    else
    {
        hfdcan->Init.DataPrescaler = 13;
        hfdcan->Init.DataSyncJumpWidth = 1;
        hfdcan->Init.DataTimeSeg1 = 2;
        hfdcan->Init.DataTimeSeg2 = 1;
    }

    switch (mode)
    {
        case CORE_CAN_MODE_NORMAL:         hfdcan->Init.Mode = FDCAN_MODE_NORMAL; break;
        case CORE_CAN_MODE_LISTEN_ONLY:     hfdcan->Init.Mode = FDCAN_MODE_BUS_MONITORING; break;
        case CORE_CAN_MODE_SELF_RECEPTION:  hfdcan->Init.Mode = FDCAN_MODE_INTERNAL_LOOPBACK; break;
        default:
            DebugUART_Print("[CAN] ch%u ERROR: invalid mode in CanTask_Open\r\n", (unsigned)channel_id);
            return -1;
    }

    if (HAL_FDCAN_Init(hfdcan) != HAL_OK)
    {
        DebugUART_Print("[CAN] ch%u ERROR: HAL_FDCAN_Init failed\r\n", (unsigned)channel_id);
        return -1;
    }

    memset(&sFilterConfig, 0, sizeof(sFilterConfig));
    sFilterConfig.IdType = FDCAN_STANDARD_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x000;
    sFilterConfig.FilterID2 = 0x000;
    if (HAL_FDCAN_ConfigFilter(hfdcan, &sFilterConfig) != HAL_OK)
    {
        DebugUART_Print("[CAN] ch%u ERROR: ConfigFilter (std) failed\r\n", (unsigned)channel_id);
        return -1;
    }

    sFilterConfig.IdType = FDCAN_EXTENDED_ID;
    sFilterConfig.FilterID1 = 0x00000000;
    sFilterConfig.FilterID2 = 0x00000000;
    if (HAL_FDCAN_ConfigFilter(hfdcan, &sFilterConfig) != HAL_OK)
    {
        DebugUART_Print("[CAN] ch%u ERROR: ConfigFilter (ext) failed\r\n", (unsigned)channel_id);
        return -1;
    }

    if (HAL_FDCAN_ConfigGlobalFilter(hfdcan,
                                     FDCAN_ACCEPT_IN_RX_FIFO0,
                                     FDCAN_ACCEPT_IN_RX_FIFO0,
                                     FDCAN_REJECT_REMOTE,
                                     FDCAN_REJECT_REMOTE) != HAL_OK)
    {
        DebugUART_Print("[CAN] ch%u ERROR: ConfigGlobalFilter failed\r\n", (unsigned)channel_id);
        return -1;
    }

    if (HAL_FDCAN_ConfigInterruptLines(hfdcan,
                                       FDCAN_IT_RX_FIFO0_NEW_MESSAGE,
                                       FDCAN_INTERRUPT_LINE0) != HAL_OK)
    {
        DebugUART_Print("[CAN] ch%u ERROR: ConfigInterruptLines failed\r\n", (unsigned)channel_id);
        return -1;
    }

    if (HAL_FDCAN_ActivateNotification(hfdcan, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK)
    {
        DebugUART_Print("[CAN] ch%u ERROR: ActivateNotification failed\r\n", (unsigned)channel_id);
        return -1;
    }

    if (HAL_FDCAN_Start(hfdcan) != HAL_OK)
    {
        DebugUART_Print("[CAN] ch%u ERROR: HAL_FDCAN_Start failed\r\n", (unsigned)channel_id);
        return -1;
    }

    g_channels[channel_id].started = 1;
    g_channels[channel_id].rx_irq_count = 0;
    g_channels[channel_id].rx_ok_count = 0;
    g_channels[channel_id].rx_queue_drop_count = 0;

    DebugUART_Print("[CAN] ch%u RX notification active\r\n", (unsigned)channel_id);
    DebugUART_Print("[CAN] ch%u channel opened, mode=%lu bitrate=%lu bit/s\r\n",
                    (unsigned)channel_id,
                    (unsigned long)hfdcan->Init.Mode,
                    (unsigned long)bitrate_bps);

    return 0;
}

int CanTask_Close(uint8_t channel_id)
{
    FDCAN_HandleTypeDef *hfdcan = ChannelHandle(channel_id);

    if (hfdcan == NULL) { return -1; }

    if (!g_channels[channel_id].started)
    {
        DebugUART_Print("[CAN] ch%u close requested, already stopped\r\n", (unsigned)channel_id);
        return 0;
    }

    if (HAL_FDCAN_Stop(hfdcan) != HAL_OK)
    {
        DebugUART_Print("[CAN] ch%u ERROR: HAL_FDCAN_Stop failed on close\r\n", (unsigned)channel_id);
        return -1;
    }

    g_channels[channel_id].started = 0;
    DebugUART_Print("[CAN] ch%u channel stopped\r\n", (unsigned)channel_id);
    return 0;
}

static void CanTask(void *argument)
{
    uint8_t channel_id = (uint8_t)(uintptr_t)argument;
    FDCAN_HandleTypeDef *hfdcan = ChannelHandle(channel_id);
    can_msg_t can_msg;
    FDCAN_TxHeaderTypeDef tx_hdr;
    uint8_t tx_data[8];

    uint32_t tx_fifo_full_count = 0;
    uint32_t add_message_fail_count = 0;

    DebugUART_Print("[CAN] ch%u CanTask started\r\n", (unsigned)channel_id);
    DebugUART_Print("[CAN] ch%u core_to_can_queue=%p can_to_core_queue=%p\r\n",
                    (unsigned)channel_id,
                    (void*)core_to_can_queues[channel_id],
                    (void*)can_to_core_queues[channel_id]);

    for (;;)
    {
        if (osMessageQueueGet(core_to_can_queues[channel_id], &can_msg, NULL, osWaitForever) == osOK)
        {
            memset(&tx_hdr, 0, sizeof(tx_hdr));
            memset(tx_data, 0, sizeof(tx_data));

            if (CanTask_BuildTxHeader(&can_msg.frame, &tx_hdr) != 0)
            {
                DebugUART_Print("[CAN] ch%u ERROR: failed to build TX header\r\n", (unsigned)channel_id);
                continue;
            }

            memcpy(tx_data, can_msg.frame.Data, can_msg.frame.Size);

            if (HAL_FDCAN_GetTxFifoFreeLevel(hfdcan) == 0U)
            {
                tx_fifo_full_count++;
                if ((tx_fifo_full_count % 200U) == 0U)
                {
                    DebugUART_Print("[CAN] ch%u ERROR: TX FIFO FULL (count=%lu) TXFQS=0x%08lX PSR=0x%08lX\r\n",
                                    (unsigned)channel_id,
                                    (unsigned long)tx_fifo_full_count,
                                    (unsigned long)hfdcan->Instance->TXFQS,
                                    (unsigned long)hfdcan->Instance->PSR);
                }
                continue;
            }

            if (HAL_FDCAN_AddMessageToTxFifoQ(hfdcan, &tx_hdr, tx_data) != HAL_OK)
            {
                add_message_fail_count++;
                if ((add_message_fail_count % 200U) == 0U)
                {
                    uint32_t err = HAL_FDCAN_GetError(hfdcan);
                    DebugUART_Print("[CAN] ch%u ERROR: AddMessageToTxFifoQ failed (count=%lu) err=0x%08lX\r\n",
                                    (unsigned)channel_id,
                                    (unsigned long)add_message_fail_count,
                                    (unsigned long)err);
                }
                continue;
            }
        }
    }
}

void CanTask_Start(uint8_t channel_id)
{
    if (channel_id >= NUM_CHANNELS)
    {
        DebugUART_Print("[CAN] ERROR: CanTask_Start invalid channel_id=%u\r\n", (unsigned)channel_id);
        return;
    }

    g_channels[channel_id].hfdcan = (channel_id == 0U) ? &hfdcan1 : &hfdcan2;

    char name[16];
    snprintf(name, sizeof(name), "CanTask%u", (unsigned)channel_id);

    const osThreadAttr_t attr = {
        .name = name,
        .stack_size = 4096,
        .priority = (osPriority_t)osPriorityAboveNormal
    };

    g_channels[channel_id].task_handle =
        osThreadNew(CanTask, (void*)(uintptr_t)channel_id, &attr);

    if (!g_channels[channel_id].task_handle)
    {
        DebugUART_Print("[CAN] ch%u ERROR: task create failed\r\n", (unsigned)channel_id);
    }
    else
    {
        DebugUART_Print("[CAN] ch%u task created\r\n", (unsigned)channel_id);
    }
}

static int CanTask_FdcanRxToCanFrame(const FDCAN_RxHeaderTypeDef *rx_hdr,
                                     const uint8_t *rx_data,
                                     can_frame_t *out_frame)
{
    if ((rx_hdr == NULL) || (out_frame == NULL)) { return -1; }

    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->Id = rx_hdr->Identifier;
    out_frame->Timestamp = 0U;

    if (rx_hdr->IdType == FDCAN_EXTENDED_ID) { out_frame->Flags |= CAN_FLAG_EXTENDED; }
    if (rx_hdr->RxFrameType == FDCAN_REMOTE_FRAME) { out_frame->Flags |= CAN_FLAG_RTR; }

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
        default: return -1;
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
    int8_t channel_id;

    if (hfdcan == NULL) { return; }
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) == 0U) { return; }

    channel_id = ChannelIdFromHandle(hfdcan);
    if (channel_id < 0) { return; }   /* прерывание от неизвестного/незарегистрированного хендла */

    g_channels[channel_id].rx_irq_count++;

    memset(&rx_hdr, 0, sizeof(rx_hdr));
    memset(rx_data, 0, sizeof(rx_data));
    memset(&can_msg, 0, sizeof(can_msg));

    if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &rx_hdr, rx_data) != HAL_OK) { return; }
    if (CanTask_FdcanRxToCanFrame(&rx_hdr, rx_data, &can_msg.frame) != 0) { return; }
    if (can_to_core_queues[channel_id] == NULL) { return; }

    g_channels[channel_id].rx_ok_count++;

    /* В callback нельзя вызывать DebugUART_Print (мьютекс) -- это
     * настоящее прерывание. Кладём без ожидания. */
    if (osMessageQueuePut(can_to_core_queues[channel_id], &can_msg, 0, 0) != osOK)
    {
        g_channels[channel_id].rx_queue_drop_count++;
    }
}

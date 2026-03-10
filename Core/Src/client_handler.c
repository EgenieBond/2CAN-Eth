/*
 * client_handler.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 */

#include "client_handler.h"
#include "cmsis_os.h"
#include "app_queues.h"
#include "debug_uart.h"
#include "fake_client_source.h"
#include <string.h>

#define CLIENT_RX_BUFFER_SIZE 128

static osThreadId_t clientHandlerTaskHandle = NULL;

static char  rx_line_buf[CLIENT_RX_BUFFER_SIZE];
static size_t rx_line_pos = 0;

/* Временный режим: fake source включён */
#define CLIENT_USE_FAKE_SOURCE 1

static void ClientHandler_SendCmdToCore(const char *cmd)
{
    eth_cmd_msg_t msg;
    memset(&msg, 0, sizeof(msg));

    strncpy(msg.data, cmd, ETH_CMD_MAX_LEN - 1);
    msg.data[ETH_CMD_MAX_LEN - 1] = '\0';

    DebugUART_Print("[CLIENT] eth_to_core_queue=%p\r\n", (void*)eth_to_core_queue);

    DebugUART_Print("[CLIENT] SendCmdToCore raw bytes: ");
    for (size_t i = 0; i < strlen(msg.data); i++)
    {
        DebugUART_Print("%02X ", (unsigned char)msg.data[i]);
    }
    DebugUART_Print("\r\n");

    osStatus_t st = osMessageQueuePut(eth_to_core_queue, &msg, 0, 0);

    DebugUART_Print("[CLIENT] osMessageQueuePut -> %d\r\n", (int)st);

    if (st == osOK)
    {
        DebugUART_Print("[CLIENT] -> CORE OK\r\n");
    }
    else
    {
        DebugUART_Print("[CLIENT] ERROR: osMessageQueuePut failed, st=%d\r\n", (int)st);
    }
}

void ClientHandler_InputBytes(const uint8_t *data, size_t len)
{
    //DebugUART_Print("[CLIENT] InputBytes len=%lu\r\n", (unsigned long)len);

    for (size_t i = 0; i < len; i++)
    {
        uint8_t b = data[i];

        /*
        DebugUART_Print("[CLIENT] byte[%lu] = 0x%02X '%c'\r\n",
                        (unsigned long)i,
                        (unsigned)b,
                        (b >= 32 && b <= 126) ? b : '.');
        */

        if (rx_line_pos < (CLIENT_RX_BUFFER_SIZE - 1))
        {
            rx_line_buf[rx_line_pos++] = (char)b;
        }
        else
        {
            DebugUART_Print("[CLIENT] RX line overflow, reset\r\n");
            rx_line_pos = 0;
            continue;
        }

        if (b == '\r')
        {
            rx_line_buf[rx_line_pos] = '\0';

            DebugUART_Print("[CLIENT] complete line: ");
            for (size_t k = 0; k < rx_line_pos; k++)
            {
                DebugUART_Print("%02X ", (unsigned char)rx_line_buf[k]);
            }
            DebugUART_Print("\r\n");

            ClientHandler_SendCmdToCore(rx_line_buf);
            rx_line_pos = 0;
        }
    }
}

void ClientHandler_PollTx(void)
{
    eth_resp_msg_t resp;
    osStatus_t st;

    st = osMessageQueueGet(core_to_eth_queue, &resp, NULL, 0);

    if (st == osOK)
    {
        DebugUART_Print("[CLIENT] got resp from CORE: %s\r\n", resp.data);

        DebugUART_Print("[CLIENT] resp raw: ");
        for (size_t i = 0; i < strlen(resp.data); i++)
        {
            DebugUART_Print("%02X ", (unsigned char)resp.data[i]);
        }
        DebugUART_Print("\r\n");

#if CLIENT_USE_FAKE_SOURCE
        DebugUART_Print("[CLIENT] TX->FAKE_CLIENT: %s\r\n", resp.data);
#else
        DebugUART_Print("[CLIENT] TX->TCP: %s\r\n", resp.data);
#endif
    }
}

static void ClientHandlerTask(void *argument)
{
    (void)argument;

    uint32_t loop_cnt = 0;

    DebugUART_Print("[CLIENT] ClientHandlerTask started\r\n");

#if CLIENT_USE_FAKE_SOURCE
    FakeClientSource_Init();
#endif

    for (;;)
    {
        loop_cnt++;

        if ((loop_cnt % 20U) == 0U)
        {
            //DebugUART_Print("[CLIENT] heartbeat loop=%lu\r\n", (unsigned long)loop_cnt);
        }

#if CLIENT_USE_FAKE_SOURCE
        FakeClientSource_Poll();
#endif
        ClientHandler_PollTx();
        osDelay(500);
    }
}

void ClientHandlerTask_Start(void)
{
    const osThreadAttr_t attr = {
        .name = "ClientHandler",
        .stack_size = 8192,
        .priority = (osPriority_t)osPriorityNormal
    };

    clientHandlerTaskHandle = osThreadNew(ClientHandlerTask, NULL, &attr);

    if (!clientHandlerTaskHandle)
    {
        DebugUART_Print("[CLIENT] ERROR: task create failed\r\n");
    }
    else
    {
        DebugUART_Print("[CLIENT] task created\r\n");
    }
}

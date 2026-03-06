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

    if (osMessageQueuePut(eth_to_core_queue, &msg, 0, 0) == osOK)
    {
    	DebugUART_Print("[CLIENT] -> CORE: %.64s", msg.data);
    }
    else
    {
        DebugUART_Print("[CLIENT] ERROR: eth_to_core_queue full\r\n");
    }
}

void ClientHandler_InputBytes(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        uint8_t b = data[i];

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
            ClientHandler_SendCmdToCore(rx_line_buf);
            rx_line_pos = 0;
        }
    }
}

void ClientHandler_PollTx(void)
{
    eth_resp_msg_t resp;

    while (osMessageQueueGet(core_to_eth_queue, &resp, NULL, 0) == osOK)
    {
#if CLIENT_USE_FAKE_SOURCE
    	DebugUART_Print("[CLIENT] TX->FAKE_CLIENT: %.64s", resp.data);
#else
        /* Потом здесь будет tcp_write()/tcp_output() */
    	DebugUART_Print("[CLIENT] TX->TCP: %.64s", resp.data);
#endif
    }
}

static void ClientHandlerTask(void *argument)
{
    (void)argument;

    DebugUART_Print("[CLIENT] ClientHandlerTask started\r\n");

#if CLIENT_USE_FAKE_SOURCE
    FakeClientSource_Init();
#endif

    for (;;)
    {
#if CLIENT_USE_FAKE_SOURCE
        FakeClientSource_Poll();
#endif
        ClientHandler_PollTx();
        osDelay(50);
    }
}

void ClientHandlerTask_Start(void)
{
    const osThreadAttr_t attr = {
        .name = "ClientHandler",
        .stack_size = 4096,
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

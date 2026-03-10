/*
 * fake_client_source.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 *  это временный источник байтового потока
 */

#include "fake_client_source.h"
#include "client_handler.h"
#include "debug_uart.h"
#include <string.h>
#include <stdint.h>

static const char *test_cmds[] =
{
    "O\r",
    "S8\r",
    "T12341122\r",
    "C\r"
};

static uint32_t cmd_index = 0;
static uint32_t tick_div = 0;

void FakeClientSource_Init(void)
{
    cmd_index = 0;
    tick_div = 0;
    DebugUART_Print("[FAKE] Fake client source init\r\n");
}

void FakeClientSource_Poll(void)
{
    tick_div++;
    if (tick_div < 20)   /* замедляем подачу команд */
        return;

    tick_div = 0;

    if (cmd_index < (sizeof(test_cmds) / sizeof(test_cmds[0])))
    {
        const char *cmd = test_cmds[cmd_index++];
        DebugUART_Print("[FAKE] inject: %s", cmd);
        ClientHandler_InputBytes((const uint8_t *)cmd, strlen(cmd));
    }
}

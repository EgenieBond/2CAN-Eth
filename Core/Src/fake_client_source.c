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
    "S8\r",
    "O\r",
    "t12321122\r",
    "t12\r",
    "t123Z122\r",
    "r1239\r",
    "C\r",
    "t12321122\r",
    "L\r",
    "t12321122\r"
};

static volatile uint32_t cmd_index = 0;
static volatile uint32_t tick_div = 0;

void FakeClientSource_Init(void)
{
    cmd_index = 0;
    tick_div = 0;
    DebugUART_Print("[FAKE] Fake client source init\r\n");
}

void FakeClientSource_Poll(void)
{
    tick_div++;

    /*
    if ((tick_div % 5U) == 0U)
    {
        DebugUART_Print("[FAKE] poll tick_div=%lu cmd_index=%lu\r\n",
                        (unsigned long)tick_div,
                        (unsigned long)cmd_index);
    }
     */

    if (tick_div < 10U)
        return;

    tick_div = 0;

    uint32_t max_cmds = (uint32_t)(sizeof(test_cmds) / sizeof(test_cmds[0]));

    if (cmd_index >= max_cmds)
    {
        //DebugUART_Print("[FAKE] no more test commands\r\n");
        return;
    }

    const char *cmd = test_cmds[cmd_index];
    DebugUART_Print("[FAKE] inject idx=%lu: %s",
                    (unsigned long)cmd_index, cmd);

    cmd_index++;

    ClientHandler_InputBytes((const uint8_t *)cmd, strlen(cmd));
}

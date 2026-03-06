/*
 * slcan_parser.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 */

#include "slcan_parser.h"
#include <string.h>
#include <stdio.h>

void Slcan_ProcessCommand(const char *cmd, char *resp, size_t resp_size)
{
    if (!cmd || !resp || resp_size == 0)
        return;

    if (strcmp(cmd, "O\r") == 0)
    {
        snprintf(resp, resp_size, "\r");
    }
    else if (strcmp(cmd, "C\r") == 0)
    {
        snprintf(resp, resp_size, "\r");
    }
    else if (strcmp(cmd, "S8\r") == 0)
    {
        snprintf(resp, resp_size, "\r");
    }
    else if (cmd[0] == 'T')
    {
        /* пока эхо/заглушка */
        snprintf(resp, resp_size, "t12341122\r");
    }
    else
    {
        snprintf(resp, resp_size, "\a");   /* bell/error в стиле SLCAN */
    }
}

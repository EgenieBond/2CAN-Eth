/*
 * slcan_parser.c
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 */

#include "slcan_parser.h"
#include <string.h>
#include <stdio.h>

static int Slcan_IsValidBitrateCode(char c)
{
    return (c >= '0' && c <= '8');
}

static int Slcan_HexCharToNibble(char c, uint8_t *out)
{
    if (!out)
        return -1;

    if ((c >= '0') && (c <= '9'))
    {
        *out = (uint8_t)(c - '0');
        return 0;
    }

    if ((c >= 'A') && (c <= 'F'))
    {
        *out = (uint8_t)(c - 'A' + 10U);
        return 0;
    }

    if ((c >= 'a') && (c <= 'f'))
    {
        *out = (uint8_t)(c - 'a' + 10U);
        return 0;
    }

    return -1;
}

static int Slcan_ParseHexByte(const char *str, uint8_t *out)
{
    uint8_t hi, lo;

    if (!str || !out)
        return -1;

    if (Slcan_HexCharToNibble(str[0], &hi) != 0)
        return -1;

    if (Slcan_HexCharToNibble(str[1], &lo) != 0)
        return -1;

    *out = (uint8_t)((hi << 4) | lo);
    return 0;
}

static int Slcan_ParseHexUint32(const char *str, size_t digits, uint32_t *out)
{
    uint32_t value = 0;
    uint8_t nibble;

    if (!str || !out)
        return -1;

    for (size_t i = 0; i < digits; i++)
    {
        if (Slcan_HexCharToNibble(str[i], &nibble) != 0)
            return -1;

        value = (value << 4) | nibble;
    }

    *out = value;
    return 0;
}

static int Slcan_ParseDlcChar(char c, uint8_t *dlc)
{
    uint8_t nibble;

    if (!dlc)
        return -1;

    if (Slcan_HexCharToNibble(c, &nibble) != 0)
        return -1;

    if (nibble > 8U)
        return -1;

    *dlc = nibble;
    return 0;
}

static int Slcan_ParseStandardFrame(const char *cmd, can_frame_t *frame, uint8_t is_rtr)
{
    uint32_t id;
    uint8_t dlc;
    size_t expected_len;
    size_t pos;

    if (!cmd || !frame)
        return -1;

    /* формат:
       tIII L DD... \r\0
       rIII L \r\0
    */

    if (Slcan_ParseHexUint32(&cmd[1], 3, &id) != 0)
        return -1;

    if (id > 0x7FFU)
        return -1;

    if (Slcan_ParseDlcChar(cmd[4], &dlc) != 0)
        return -1;

    if (is_rtr)
    {
        expected_len = 1U + 3U + 1U + 1U; /* r + III + L + \r */
        if ((strlen(cmd) != expected_len) || (cmd[5] != '\r'))
            return -1;
    }
    else
    {
        expected_len = 1U + 3U + 1U + ((size_t)dlc * 2U) + 1U; /* t + III + L + data + \r */
        if (strlen(cmd) != expected_len)
            return -1;

        if (cmd[expected_len - 1U] != '\r')
            return -1;
    }

    memset(frame, 0, sizeof(*frame));
    frame->Id = id;
    frame->Size = dlc;
    frame->Flags = 0U;
    frame->Timestamp = 0U;

    if (is_rtr)
    {
        frame->Flags |= CAN_FLAG_RTR;
        return 0;
    }

    pos = 5U;
    for (uint8_t i = 0; i < dlc; i++)
    {
        if (Slcan_ParseHexByte(&cmd[pos], &frame->Data[i]) != 0)
            return -1;
        pos += 2U;
    }

    return 0;
}

static int Slcan_ParseExtendedFrame(const char *cmd, can_frame_t *frame, uint8_t is_rtr)
{
    uint32_t id;
    uint8_t dlc;
    size_t expected_len;
    size_t pos;

    if (!cmd || !frame)
        return -1;

    /* формат:
       TIIIIIIII L DD... \r\0
       RIIIIIIII L \r\0
    */

    if (Slcan_ParseHexUint32(&cmd[1], 8, &id) != 0)
        return -1;

    if (Slcan_ParseDlcChar(cmd[9], &dlc) != 0)
        return -1;

    if (is_rtr)
    {
        expected_len = 1U + 8U + 1U + 1U; /* R + IIIIIIII + L + \r */
        if ((strlen(cmd) != expected_len) || (cmd[10] != '\r'))
            return -1;
    }
    else
    {
        expected_len = 1U + 8U + 1U + ((size_t)dlc * 2U) + 1U; /* T + ID + DLC + data + \r */
        if (strlen(cmd) != expected_len)
            return -1;

        if (cmd[expected_len - 1U] != '\r')
            return -1;
    }

    memset(frame, 0, sizeof(*frame));
    frame->Id = id;
    frame->Size = dlc;
    frame->Flags = CAN_FLAG_EXTENDED;
    frame->Timestamp = 0U;

    if (is_rtr)
    {
        frame->Flags |= CAN_FLAG_RTR;
        return 0;
    }

    pos = 10U;
    for (uint8_t i = 0; i < dlc; i++)
    {
        if (Slcan_ParseHexByte(&cmd[pos], &frame->Data[i]) != 0)
            return -1;
        pos += 2U;
    }

    return 0;
}

int Slcan_ParseCommand(const char *cmd, slcan_cmd_t *out)
{
    if (!cmd || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    if (strcmp(cmd, "O\r") == 0)
    {
        out->type = SLCAN_CMD_OPEN;
        return 0;
    }

    if (strcmp(cmd, "C\r") == 0)
    {
        out->type = SLCAN_CMD_CLOSE;
        return 0;
    }

    if (strcmp(cmd, "L\r") == 0)
    {
        out->type = SLCAN_CMD_LISTEN;
        return 0;
    }

    if (strcmp(cmd, "Y\r") == 0)
    {
        out->type = SLCAN_CMD_SELF_RECEPTION;
        return 0;
    }

    if ((cmd[0] == 'S') &&
        Slcan_IsValidBitrateCode(cmd[1]) &&
        (cmd[2] == '\r') &&
        (cmd[3] == '\0'))
    {
        out->type = SLCAN_CMD_SET_BITRATE;
        out->bitrate_code = (uint8_t)(cmd[1] - '0');
        return 0;
    }

    if (cmd[0] == 't')
    {
        if (Slcan_ParseStandardFrame(cmd, &out->frame, 0U) != 0)
            return -1;

        out->type = SLCAN_CMD_SEND_FRAME;
        return 0;
    }

    if (cmd[0] == 'T')
    {
        if (Slcan_ParseExtendedFrame(cmd, &out->frame, 0U) != 0)
            return -1;

        out->type = SLCAN_CMD_SEND_FRAME;
        return 0;
    }

    if (cmd[0] == 'r')
    {
        if (Slcan_ParseStandardFrame(cmd, &out->frame, 1U) != 0)
            return -1;

        out->type = SLCAN_CMD_SEND_FRAME;
        return 0;
    }

    if (cmd[0] == 'R')
    {
        if (Slcan_ParseExtendedFrame(cmd, &out->frame, 1U) != 0)
            return -1;

        out->type = SLCAN_CMD_SEND_FRAME;
        return 0;
    }

    return -1;
}

int Slcan_FormatFrame(const can_frame_t *frame, char *resp, size_t resp_size)
{
    int written;
    size_t pos = 0U;
    char type_char;

    if (!frame || !resp || (resp_size == 0U))
        return -1;

    memset(resp, 0, resp_size);

    if (frame->Size > 8U)
        return -1;

    if (frame->Flags & CAN_FLAG_EXTENDED)
    {
        type_char = (frame->Flags & CAN_FLAG_RTR) ? 'R' : 'T';

        written = snprintf(&resp[pos], resp_size - pos, "%c%08lX%1X",
                           type_char,
                           (unsigned long)frame->Id,
                           (unsigned)frame->Size);
    }
    else
    {
        if (frame->Id > 0x7FFU)
            return -1;

        type_char = (frame->Flags & CAN_FLAG_RTR) ? 'r' : 't';

        written = snprintf(&resp[pos], resp_size - pos, "%c%03lX%1X",
                           type_char,
                           (unsigned long)frame->Id,
                           (unsigned)frame->Size);
    }

    if (written < 0)
        return -1;

    if ((size_t)written >= (resp_size - pos))
        return -1;

    pos += (size_t)written;

    if ((frame->Flags & CAN_FLAG_RTR) == 0U)
    {
        for (uint8_t i = 0; i < frame->Size; i++)
        {
            written = snprintf(&resp[pos], resp_size - pos, "%02X",
                               (unsigned)frame->Data[i]);
            if (written < 0)
                return -1;

            if ((size_t)written >= (resp_size - pos))
                return -1;

            pos += (size_t)written;
        }
    }

    if ((pos + 2U) > resp_size)
        return -1;

    resp[pos++] = '\r';
    resp[pos] = '\0';

    return 0;
}

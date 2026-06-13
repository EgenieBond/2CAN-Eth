/*
 * slcan_types.h
 *
 *  Created on: Mar 10, 2026
 *      Author: Egenie
 */

#ifndef SLCAN_TYPES_H
#define SLCAN_TYPES_H

#include <stdint.h>
#include "can_types.h"

typedef enum
{
    SLCAN_CMD_NONE = 0,
    SLCAN_CMD_OPEN,
    SLCAN_CMD_CLOSE,
    SLCAN_CMD_LISTEN,
    SLCAN_CMD_SELF_RECEPTION,
    SLCAN_CMD_SET_BITRATE,
    SLCAN_CMD_SEND_FRAME
} slcan_cmd_type_t;

typedef struct
{
    slcan_cmd_type_t type;
    uint8_t bitrate_code;     /* если пришёл классический S0..S8 */
    uint32_t bitrate_bps;     /* итоговый битрейт в бит/с */
    can_frame_t frame;
} slcan_cmd_t;

#endif /* SLCAN_TYPES_H */

/*
 * can_types.h
 *
 *  Created on: Mar 10, 2026
 *      Author: Egenie
 */

#ifndef CAN_TYPES_H
#define CAN_TYPES_H

#include <stdint.h>

/* Флаги CAN-кадра */
#define CAN_FLAG_EXTENDED   0x01U   /* 29-bit ID */
#define CAN_FLAG_RTR        0x02U   /* Remote Transmission Request */
#define CAN_FLAG_SELF_RX    0x04U   /* Self reception / loopback marker if needed */

typedef struct
{
    uint32_t Id;         /* CAN identifier */
    uint8_t  Size;       /* DLC: 0..8 */
    uint8_t  Data[8];    /* payload */
    uint8_t  Flags;      /* EXT / RTR / etc */
    uint16_t Timestamp;  /* optional timestamp */
} can_frame_t;

#endif /* CAN_TYPES_H */

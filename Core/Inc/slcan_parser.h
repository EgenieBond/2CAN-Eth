/*
 * slcan_parser.h
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 */

#ifndef INC_SLCAN_PARSER_H_
#define INC_SLCAN_PARSER_H_

#include <stddef.h>
#include "slcan_types.h"
#include "can_types.h"

int Slcan_ParseCommand(const char *cmd, slcan_cmd_t *out);
int Slcan_FormatFrame(const can_frame_t *frame, char *resp, size_t resp_size);

#endif /* INC_SLCAN_PARSER_H_ */

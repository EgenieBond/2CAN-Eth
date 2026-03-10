/*
 * slcan_parser.h
 *
 *  Created on: Mar 6, 2026
 *      Author: Egenie
 */

#ifndef INC_SLCAN_PARSER_H_
#define INC_SLCAN_PARSER_H_

#include <stddef.h>

void Slcan_ProcessCommand(const char *cmd, char *resp, size_t resp_size);

#endif /* INC_SLCAN_PARSER_H_ */

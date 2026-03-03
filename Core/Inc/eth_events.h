/*
 * eth_events.h
 *
 *  Created on: Mar 2, 2026
 *      Author: Egenie
 */

#ifndef INC_ETH_EVENTS_H_
#define INC_ETH_EVENTS_H_

#pragma once

#include "cmsis_os.h"
#include <stdint.h>

/* App-level event flags for Ethernet */
#define APP_ETH_EVT_LINK_UP   (1UL << 0)

/* Защита от “маска стала 0” */
#if (APP_ETH_EVT_LINK_UP == 0)
#error "APP_ETH_EVT_LINK_UP must not be 0"
#endif

/* Global handle (defined in exactly one .c file) */
extern osEventFlagsId_t g_ethLinkEvt;

#endif /* INC_ETH_EVENTS_H_ */

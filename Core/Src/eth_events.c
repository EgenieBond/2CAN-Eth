/*
 * eth_events.c
 *
 *  Created on: Mar 2, 2026
 *      Author: Egenie
 */

#include "eth_events.h"

/* Глобальный handle (определение ровно ОДИН раз) */
osEventFlagsId_t g_ethLinkEvt = NULL;

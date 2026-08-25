/*
 * eth_loopback_test.h
 *
 * Диагностический тест: плата отправляет данные сама себе через
 * внутренний loopback (LAN8742), без выхода в кабель
 * и без участия внешнего клиента (ПК).
 */

#ifndef ETH_LOOPBACK_TEST_H_
#define ETH_LOOPBACK_TEST_H_

/* Запускает PHY-loopback тест: включает loopback-режим в PHY,
 * затем стартует отдельную FreeRTOS-задачу, которая шлёт кадры
 * через штатный TX-путь и считает, сколько дошло обратно через
 * штатный RX-путь. */
void EthLoopbackTest_Start(void);

#endif /* ETH_LOOPBACK_TEST_H_ */

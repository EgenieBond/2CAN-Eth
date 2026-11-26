# 2CAN-Eth
Автор: Бондаренко Е.А.

## Ветки
- **main** - стабильная версия
- **Fixing** - ветка для исправлений и тестирования всего проекта
- **Test_Simple_TCP** - ветка для тестирования максимальной скорости Ethernet
- **was-stable-4-hours** - ветка с версией, которая отработала на Модуле 3 часа без ошибок

## Последняя версия (26.11)
- проверена поддержка всех стандартных скоростей
- непрерывный тест (передача ПК 2001 -> FDCAN1 -> FDCAN2 -> ПК 2002) отработал 4 часа без ошибок

## В разработке
- Был проведен тест, когда данные отправлялись между ПК и платой только по Ethernet, без CAN. Обнаружилась проблема в таймере, который закрывает соединение при Zero Window
- для этого в файле client_handler.c теперь есть флаг #define ETH_BENCH_MODE (1 - только Eth, 0 - вместе с CAN)

## Доп. файлы проекта
- check_2_netcans_simple.ру - скрипт для теста на базе vs_can_lib
- bench_check.py - скрипт для теста только Ethernet

## Используемые версии
- плата STM32 NUCLEO H723ZG
- STM32CubeIDE Version: 1.17.0
- LWIP.Version=v2.1.2_Cube
- LWIP0.BSP.component=LAN8742
- Mcu.CPN=STM32H723ZGT6
- Mcu.IP0=CORTEX_M7
- MxCube.Version=6.13.0
- ProjectManager.FirmwarePackage=STM32Cube FW_H7 V1.12.0
- PyCharm на Python 3.10 с использованием vs_can_lib
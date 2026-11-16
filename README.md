# 2CAN-Eth
Автор: Бондаренко Е.А.

## Ветки
- **main** - стабильная версия
- **Fixing** - ветка для исправлений и тестирования всего проекта
- **Test_Simple_TCP** - ветка для тестирования скорости Ethernet

## Последняя версия (12.11)
- в проект был включен FDCAN2 для работы с 2мя каналами
- проверена поддержка всех стандартных скоростей
- непрерывный тест (отправка пачки 1000 кадров без пауз) отработал 4 часа без потерь кадров и ошибок

## Новые файлы (скоро появятся)
tcp_blast_test.h и tcp_blast_test.c, они нужны только для проверки пропускной способности сетевой карты. Активируются при установлении флага в main.c: #define TCP_BLAST_TEST 1, примерно 115 строка.

## Доп. файлы проекта
- check_2_netcans_simple.ру - скрипт для теста на базе vs_can_lib
- raw_socket_test.ру - скрипт для теста на базе raw sockets
- замер_скорости.xlsx - таблица замеров скоростей передачи между STM32 и NETCAN

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
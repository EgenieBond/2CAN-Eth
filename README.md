# 2CAN-Eth - ветка для временной копии проекта

## Версия
Версия проекта от 3 мая

## Ветки
- **main** - стабильная версия
- **Fixing** - ветка для исправлений и тестирования всего проекта
- **Fix_TCP_Server** - ветка для исправлений и тестирования части с TCP-сервером

## Что сейчас готово
1. Архитектура из 3 модулей: Ethernet / Core / CAN
2. FreeRTOS + задачи + очереди между модулями
3. TCP-сервер (lwIP, порт 2001), 1 клиент. Приём TCP как потока + разбор по \r
4. Передача команд в ядро (eth_to_core_queue), парсинг SLCAN (базовые команды: C, Sx, O, t)
5. Базовое управление состоянием канала (open/close, хранение скорости)
6. Формирование can_frame_t и передача в CAN (core_to_can_queue)
7. Отправка через FDCAN (TX работает), приём CAN-кадра (RX работает)
8. Отправка ответа обратно в TCP (core_to_eth_queue)

## Вывод в путти
[ETH] EthernetTask ENTER (HAL_UART_Transmit)
[MEM] _sbss=0x240000C0 _ebss=0x240289E8
[MEM] lwip_heap: 0x24028A00 .. 0x24038A00
[MEM] LWIP_RAM_HEAP_POINTER=0x24028A00
[ETH] waiting LINK UP (event)... evt=0x24004568 mask=0x00000001
[CAN] CanTask started
[CAN] core_to_can_queue=0x24006320 can_to_core_queue=0x240063f8
[ETH] EthernetTask thread created
[ETH] HAL_ETH_Start_IT OK, speed=100M duplex=FULL
[ETH] link_thread: scheduling LINK UP in tcpip_thread
[ETH] netif_link_up_in_tcpip ENTER flags_before=0x1A
[LWIP] netif is DOWN
[LWIP] link is UP
[ETH] tcpip: netif link UP + netif UP
[ETH] tcpip: flags_after=0x1F
[ETH] tcpip: input=0x8010a31 output=0x8019a6d linkoutput=0x800b925
[ETH] tcpip: hwaddr=00:80:E1:00:00:00
[ETH] tcpip: ip=192.168.0.17
[ETH] netif_link_up_in_tcpip EXIT
[ETH] wait returned=0x00000001 now_get=0x00000000
[ETH] osEventFlagsWait OK: flags=0x00000001
[ETH] LINK UP (task sees it)
[ETH] My IP: 192.168.0.17
[ETH] netif flags=0x1F
[ETH] netif MAC: 00:80:E1:00:00:00
[ETH] IP check FAIL
[ETH] starting RAW TCP server...
[ETH] tcpip_callback(raw server) -> 0
[TCP] init cb: running in tcpip_thread
[TCP] Listening on port 2001
[TCP] init cb: RawTcpServer_Init done
[TCP] ACCEPT from 192.168.0.1:56354
[CAN] close requested, controller already stopped
[CORE] channel CLOSED
[CORE] parsed bitrate: S8 -> 1000000 bit/s
[CORE] bitrate set: S8 -> 1000000 bit/s
[CAN] bitrate applied: 1000000 bit/s -> Presc=1 SJW=1 TSEG1=16 TSEG2=8
[CAN] channel opened, mode=3 bitrate=1000000 bit/s
[CAN] NBTP=0x00000F07
[CAN] PSR=0x0000070F
[CAN] CCCR=0x000010A0
[CORE] channel SELF RECEPTION
[CORE] active bitrate: S8 -> 1000000 bit/s
[TCP] Client disconnected (p==NULL)
[TCP] ACCEPT from 192.168.0.1:56358
[CAN] channel stopped
[CORE] channel CLOSED
[CORE] parsed bitrate: S8 -> 1000000 bit/s
[CORE] bitrate set: S8 -> 1000000 bit/s
[CAN] bitrate applied: 1000000 bit/s -> Presc=1 SJW=1 TSEG1=16 TSEG2=8
[CAN] channel opened, mode=3 bitrate=1000000 bit/s
[CAN] NBTP=0x00000F07
[CAN] PSR=0x00000708
[CAN] CCCR=0x000010A0
[CORE] channel SELF RECEPTION
[CORE] active bitrate: S8 -> 1000000 bit/s
[TCP] Client disconnected (p==NULL)
[TCP] ACCEPT from 192.168.0.1:56360
[CAN] channel stopped
[CORE] channel CLOSED
[CORE] parsed bitrate: S8 -> 1000000 bit/s
[CORE] bitrate set: S8 -> 1000000 bit/s
[CAN] bitrate applied: 1000000 bit/s -> Presc=1 SJW=1 TSEG1=16 TSEG2=8
[CAN] channel opened, mode=3 bitrate=1000000 bit/s
[CAN] NBTP=0x00000F07
[CAN] PSR=0x00000708
[CAN] CCCR=0x000010A0
[CORE] channel SELF RECEPTION
[CORE] active bitrate: S8 -> 1000000 bit/s
[TCP] Client disconnected (p==NULL)
[TCP] ACCEPT from 192.168.0.1:56364
[CAN] channel stopped
[CORE] channel CLOSED
[CORE] parsed bitrate: S8 -> 1000000 bit/s
[CORE] bitrate set: S8 -> 1000000 bit/s
[CAN] bitrate applied: 1000000 bit/s -> Presc=1 SJW=1 TSEG1=16 TSEG2=8
[CAN] channel opened, mode=3 bitrate=1000000 bit/s
[CAN] NBTP=0x00000F07
[CAN] PSR=0x00000708
[CAN] CCCR=0x000010A0
[CORE] channel SELF RECEPTION
[CORE] active bitrate: S8 -> 1000000 bit/s
[ETH] DMA error: hal_err=0x00000008 dma_err=0x00004480
[ETH] DMA error: hal_err=0x00000008 dma_err=0x00004480
[ETH] DMA error: hal_err=0x00000008 dma_err=0x00004480
[ETH] DMA error: hal_err=0x00000008 dma_err=0x00004480
[TCP] Client disconnected (p==NULL)

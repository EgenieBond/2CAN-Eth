# 2CAN-Eth - ветка для временной копии проекта

## Версия
Версия проекта от 22 апреля

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

## Чего пока нет
1. Полная реализация всех команд:
- L (listen-only)
- Y (self reception)
- T, r, R
2. Применение скорости при O
3. Финальная реализация RX через: HAL_FDCAN_RxFifo0Callback() (без polling)
4. Обработка ошибок: CAN (TX fail, bus state, invalid state)

## Вывод в путти
=== SYSTEM START ===
[CPU] CCR = 0x00060200
[ETH] MAC: 00:80:E1:00:00:00
[ETH] SYSCFG->PMCR = 0x03800000
[EVT] g_ethLinkEvt created = 0x24004540
[EVT] mask APP_ETH_EVT_LINK_UP=0x00000001
[PHY] BMCR  = 0x3000
[PHY] BMSR  = 0x7809
[PHY] PSCSR = 0x0040
[ETH] PHYLinkState(initial)=1
[PHY] BMCR  = 0x3000
[PHY] BMSR  = 0x7809
[PHY] PSCSR = 0x0040
[ETH] initial PHY link DOWN
[ETH] low_level_init done, waiting for ethernet_link_thread to control link stat                            e
[LWIP] Ethernet link thread created
[LWIP] IP: 10.0.0.100
[LWIP] NETMASK: 255.255.255.0
[LWIP] GW: 10.0.0.1
[FDCAN] started OK
[FDCAN] PSR=0x0000070F
[FDCAN] CCCR=0x000010A0
[FDCAN] NBTP=0x00090B06
[BOOT] after FDCAN1 init
[APP] eth_to_core_queue=0x24005e48 item=64
[APP] core_to_eth_queue=0x240060a0 item=64
[APP] core_to_can_queue=0x240062f8 item=16
[APP] can_to_core_queue=0x240063d0 item=16
[APP] Queues created OK
[CLIENT] task created
[APP] EthApp_Init done
[CORE] task created
[CAN] task created

[ETH] EthernetTask ENTER (HAL_UART_Transmit)
[MEM] _sbss=0x240000A0 _ebss=0x240289A8
[MEM] lwip_heap: 0x240289C0 .. 0x240389C0
[MEM] LWIP_RAM_HEAP_POINTER=0x240289C0
[ETH] waiting LINK UP (event)... evt=0x24004540 mask=0x00000001
[CORE] CoreTask started
[CORE] eth_to_core_queue=0x24005e48 core_to_eth_queue=0x240060a0
[CORE] core_to_can_queue=0x240062f8 can_to_core_queue=0x240063d0
[ETH] EthernetTask thread created
[CLIENT] ClientHandlerTask started
[CAN] CanTask started
[CAN] core_to_can_queue=0x240062f8 can_to_core_queue=0x240063d0
[ETH] HAL_ETH_Start_IT OK, speed=100M duplex=FULL
[ETH] link_thread: scheduling LINK UP in tcpip_thread
[ETH] netif_link_up_in_tcpip ENTER flags_before=0x1A
[LWIP] netif is DOWN
[LWIP] link is UP
[ETH] tcpip: netif link UP + netif UP
[ETH] tcpip: flags_after=0x1F
[ETH] tcpip: input=0x80107d1 output=0x801980d linkoutput=0x800b6c9
[ETH] tcpip: hwaddr=00:80:E1:00:00:00
[ETH] tcpip: ip=10.0.0.100
[ETH] netif_link_up_in_tcpip EXIT
[ETH] wait returned=0x00000001 now_get=0x00000000
[ETH] osEventFlagsWait OK: flags=0x00000001
[ETH] LINK UP (task sees it)
[ETH] My IP: 10.0.0.100
[ETH] netif flags=0x1F
[ETH] netif MAC: 00:80:E1:00:00:00
[ETH] IP check OK: netif has 10.0.0.100
[ETH] starting RAW TCP server...
[ETH] tcpip_callback(raw server) -> 0
[TCP] init cb: running in tcpip_thread
[TCP] Listening on port 2001
[TCP] init cb: RawTcpServer_Init done
[TCP] ACCEPT from 10.0.0.2:50677
[TCP] RX chunk len=17
[CORE] got cmd raw: 43 0D
[CORE] parse OK, type=CLOSE
[CORE] channel CLOSED
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CORE] got cmd raw: 53 38 0D
[CORE] parse OK, type=SET_BITRATE
[CORE] parsed bitrate: S8
[CORE] bitrate set: S8
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CORE] got cmd raw: 4F 0D
[CORE] parse OK, type=OPEN
[CORE] channel OPEN, bitrate=S8
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CORE] got cmd raw: 74 31 32 33 32 31 31 32 32 0D
[CORE] parse OK, type=SEND_FRAME
[CORE] parsed frame: ID=0x00000123 DLC=2 FLAGS=0x00 DATA=11 22
[CORE] frame queued to CAN
[CORE] frame: ID=0x00000123 DLC=2 FLAGS=0x00
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CAN] frame from CORE: DATA ID=0x00000123 DLC=2 FLAGS=0x00 DATA=11 22
[CAN] TX header prepared:
[CAN]   IdType     = STANDARD
[CAN]   Identifier = 0x00000123
[CAN]   FrameType  = DATA
[CAN]   DataLength = 0x00000002
[CAN] payload prepared: 11 22
[CAN] TX queued into FDCAN FIFO OK
[TCP] Client disconnected (p==NULL)
[CORE] CAN RX DATA: ID=0x00000123 DLC=2 FLAGS=0x00 DATA=11 22
[CORE] formatted SLCAN: t12321122
[CORE] CAN RX -> ETH OK
[CAN] TXFQS=0x00010103
[CAN] RXF0S=0x00010100
[CAN] IR=0x00000800
[CAN] PSR=0x00000708
[CAN] RX FIFO0 fill level after TX = 0
[TCP] Async send skipped: no active client
[CLIENT] ERROR: TX->TCP async rc=-3
[TCP] ACCEPT from 10.0.0.2:51459
[TCP] RX chunk len=17
[CORE] got cmd raw: 43 0D
[CORE] parse OK, type=CLOSE
[CORE] channel CLOSED
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CORE] got cmd raw: 53 38 0D
[CORE] parse OK, type=SET_BITRATE
[CORE] parsed bitrate: S8
[CORE] bitrate set: S8
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CORE] got cmd raw: 4F 0D
[CORE] parse OK, type=OPEN
[CORE] channel OPEN, bitrate=S8
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CORE] got cmd raw: 74 31 32 33 32 31 31 32 32 0D
[CORE] parse OK, type=SEND_FRAME
[CORE] parsed frame: ID=0x00000123 DLC=2 FLAGS=0x00 DATA=11 22
[CORE] frame queued to CAN
[CORE] frame: ID=0x00000123 DLC=2 FLAGS=0x00
[CORE] resp str:
[CORE] resp raw: 0D
[CAN] frame from CORE: DATA ID=0x00000123 DLC=2 FLAGS=0x00 DATA=11 22
[CAN] TX header prepared:
[CAN]   IdType     = STANDARD
[CAN]   Identifier = 0x00000123
[CAN]   FrameType  = DATA
[CAN]   DataLength = 0x00000002
[CAN] payload prepared: 11 22 [CORE] processed OK

[CAN] TX queued into FDCAN FIFO OK
[CORE] CAN RX DATA: ID=0x00000123 DLC=2 FLAGS=0x00 DATA=11 22
[CORE] formatted SLCAN: t12321122
[CORE] CAN RX -> ETH OK
[CAN] TXFQS=0x00020203
[CAN] RXF0S=0x00020200
[CAN] IR=0x00000800
[CAN] PSR=0x00000708
[CAN] RX FIFO0 fill level after TX = 0
[TCP] Client disconnected (p==NULL)

# 2CAN-Eth - ветка для временной копии проекта

## Версия
Версия проекта от 11 марта

## Ветки
- **main** - стабильная версия
- **Fixing** - ветка для исправлений и тестирования

## Отличие от ветки main
- уже работает цепочка:
CAN frame -> Core -> SLCAN string -> Ethernet response
- То есть функция обратного преобразования тоже живая. Но пока без реального CAN

## Дальнейшая работа
Сделать реальный TCP-сервер (сейчас вместо него FakeClientSource_Poll)

## Вывод текущего лога через UART:
=== USART3 INIT OK (PD8/PD9) ===
[BOOT] after UART init
[RESET] RCC->RSR=0x00420000

=== SYSTEM START ===
CPU Clock: 64000000 Hz
Starting FreeRTOS...
[BOOT] after osKernelInitialize
[HEAP] free before EthTask_Start: 57224
[TASK] default task started
[ETH] ethernetif.c BUILD TAG: AAA_1
[ETH] Initializing Ethernet hardware...
[ETH] MAC: 00:80:E1:00:00:00
[EVT] g_ethLinkEvt created = 0x24003508
[EVT] mask APP_ETH_EVT_LINK_UP=0x00000001
[ETH] initial PHY link DOWN
[LWIP] Ethernet link thread created
[LWIP] IP: 10.0.0.100
[HEAP] free after LWIP init: 45968
[HEAP] min ever free:      45968
[LWIP] MX_LWIP_Init done
[APP] eth_to_core_queue=0x24004e10 item=64
[APP] core_to_eth_queue=0x24005068 item=64
[APP] core_to_can_queue=0x240052c0 item=16
[APP] can_to_core_queue=0x24005398 item=16
[APP] Queues created OK
[CLIENT] task created
[APP] EthApp_Init done
[CLIENT] ClientHandlerTask started
[FAKE] Fake client source init
[CORE] CoreTask started
[CORE] eth_to_core_queue=0x24004e10 core_to_eth_queue=0x24005068
[CORE] task created
[CAN] task created

[ETH] EthernetTask ENTER (HAL_UART_Transmit)
[ETH] Ethernet task started (DebugUART)
[MEM] _sbss=0x240000C8 _ebss=0x24018294
[MEM] lwip_heap: 0x240182A0 .. 0x240282A0
[MEM] LWIP_RAM_HEAP_POINTER=0x240182A0
[ETH] waiting LINK UP (event)... evt=0x24003508 mask=0x00000001
[CORE] core_to_can_queue=0x240052c0 can_to_core_queue=0x24005398
[CAN] CanTask started
[CAN] core_to_can_queue=0x240052c0 can_to_core_queue=0x24005398
[ETH] EthernetTask thread created
[LWIP] netif is UP
[LWIP] link is UP
[ETH] wait returned=0x00000001 now_get=0x00000000
[ETH] osEventFlagsWait OK: flags=0x00000001
[ETH] LINK UP (task sees it)
[ETH] My IP: 10.0.0.100
[ETH] TEMP: RAW TCP server start is disabled
[CLIENT] complete line: 53 38 0D
[CLIENT] eth_to_core_queue=0x24004e10
[CLIENT] SendCmdToCore raw bytes: 53 38 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 53 38 0D
[CORE] parse OK, type=SET_BITRATE
[CORE] parsed bitrate: S8
[CORE] bitrate set: S8
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 0D
[CLIENT] TX->FAKE_CLIENT:
[CLIENT] complete line: 4F 0D
[CLIENT] eth_to_core_queue=0x24004e10
[CLIENT] SendCmdToCore raw bytes: 4F 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 4F 0D
[CORE] parse OK, type=OPEN
[CORE] channel OPEN, bitrate=S8
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 0D
[CLIENT] TX->FAKE_CLIENT:
[CLIENT] complete line: 74 31 32 33 32 31 31 32 32 0D
[CLIENT] eth_to_core_queue=0x24004e10
[CLIENT] SendCmdToCore raw bytes: 74 31 32 33 32 31 31 32 32 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
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
[CAN]   DataLength = 2
[CAN] payload prepared: 11 22
[CAN] TEMP: frame is ready for future HAL_FDCAN_AddMessageToTxFifoQ()
[CAN] DEBUG: frame looped back to can_to_core_queue
[CORE] CAN RX DATA: ID=0x00000123 DLC=2 FLAGS=0x00 DATA=11 22
[CORE] formatted SLCAN: t12321122
[CORE] CAN RX -> ETH OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 0D
[CLIENT] TX->FAKE_CLIENT:
[CLIENT] got resp from CORE: t12321122
[CLIENT] resp raw: 74 31 32 33 32 31 31 32 32 0D
[CLIENT] TX->FAKE_CLIENT: t12321122
[CLIENT] complete line: 74 31 32 0D
[CLIENT] eth_to_core_queue=0x24004e10
[CLIENT] SendCmdToCore raw bytes: 74 31 32 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 74 31 32 0D
[CORE] parse ERROR: unsupported or invalid command
[CORE] resp str:
[CORE] resp raw: 07
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 07
[CLIENT] TX->FAKE_CLIENT:
[CLIENT] complete line: 74 31 32 33 5A 31 32 32 0D
[CLIENT] eth_to_core_queue=0x24004e10
[CLIENT] SendCmdToCore raw bytes: 74 31 32 33 5A 31 32 32 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 74 31 32 33 5A 31 32 32 0D
[CORE] parse ERROR: unsupported or invalid command
[CORE] resp str:
[CORE] resp raw: 07
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 07
[CLIENT] TX->FAKE_CLIENT:
[CLIENT] complete line: 72 31 32 33 39 0D
[CLIENT] eth_to_core_queue=0x24004e10
[CLIENT] SendCmdToCore raw bytes: 72 31 32 33 39 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 72 31 32 33 39 0D
[CORE] parse ERROR: unsupported or invalid command
[CORE] resp str:
[CORE] resp raw: 07
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 07
[CLIENT] TX->FAKE_CLIENT:
[CLIENT] complete line: 43 0D
[CLIENT] eth_to_core_queue=0x24004e10
[CLIENT] SendCmdToCore raw bytes: 43 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 43 0D
[CORE] parse OK, type=CLOSE
[CORE] channel CLOSED
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 0D
[CLIENT] TX->FAKE_CLIENT:
[CLIENT] complete line: 74 31 32 33 32 31 31 32 32 0D
[CLIENT] eth_to_core_queue=0x24004e10
[CLIENT] SendCmdToCore raw bytes: 74 31 32 33 32 31 31 32 32 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 74 31 32 33 32 31 31 32 32 0D
[CORE] parse OK, type=SEND_FRAME
[CORE] parsed frame: ID=0x00000123 DLC=2 FLAGS=0x00 DATA=11 22
[CORE] ERROR: cannot send frame, channel is CLOSED
[CORE] resp str:
[CORE] resp raw: 07
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 07
[CLIENT] TX->FAKE_CLIENT:
[CLIENT] complete line: 4C 0D
[CLIENT] eth_to_core_queue=0x24004e10
[CLIENT] SendCmdToCore raw bytes: 4C 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 4C 0D
[CORE] parse OK, type=LISTEN
[CORE] channel LISTEN ONLY, bitrate=S8
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 0D
[CLIENT] TX->FAKE_CLIENT:
[CLIENT] complete line: 74 31 32 33 32 31 31 32 32 0D
[CLIENT] eth_to_core_queue=0x24004e10
[CLIENT] SendCmdToCore raw bytes: 74 31 32 33 32 31 31 32 32 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 74 31 32 33 32 31 31 32 32 0D
[CORE] parse OK, type=SEND_FRAME
[CORE] parsed frame: ID=0x00000123 DLC=2 FLAGS=0x00 DATA=11 22
[CORE] ERROR: cannot send frame in LISTEN ONLY mode
[CORE] resp str:
[CORE] resp raw: 07
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 07
[CLIENT] TX->FAKE_CLIENT:


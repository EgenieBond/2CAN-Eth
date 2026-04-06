# 2CAN-Eth - ветка для временной копии проекта

## Версия
Версия проекта от 6 апреля
TCP-сервер, приём SLCAN-команд, обработка команд и возврат ответов работают корректно.

## Ветки
- **main** - стабильная версия
- **Fixing** - ветка для исправлений и тестирования всего проекта
- **Fix_TCP_Server** - ветка для исправлений и тестирования части с TCP-сервером

## Что сейчас готово
- Ethernet на STM32 поднят, плата получает статический IP 10.0.0.100
- линк поднимается, плата отвечает на ping, TCP-сервер на порту 2001 работает
- ПК успешно подключается (в логах есть TCP ACCEPT)
- Приём данных по TCP работает, сервер принимает байты от клиента
- ClientHandler собирает поток до символа \r
- команды корректно выделяются даже если пришли подряд в одном TCP-пакете
- Связка TCP -> ClientHandler -> Core работает
- CoreTask команды читает и обрабатывает, SLCAN-парсер работает
- Передача ответов обратно по TCP работает, core_to_eth_queue читается, ответы уходят клиенту

## Чего пока нет
- реальная отправка в физический FDCAN
- реальный приём из шины CAN через прерывания/HAL

## Вывод в путти
=== SYSTEM START ===
[CPU] CCR before LWIP = 0x00060200
[ETH] MAC: 00:80:E1:00:00:00
[ETH] SYSCFG->PMCR = 0x03800000
[EVT] g_ethLinkEvt created = 0x24003540
[EVT] mask APP_ETH_EVT_LINK_UP=0x00000001
[PHY] BMCR  = 0x3000
[PHY] BMSR  = 0x7809
[PHY] PSCSR = 0x0040
[ETH] PHYLinkState(initial)=1
[PHY] BMCR  = 0x3000
[PHY] BMSR  = 0x7809
[PHY] PSCSR = 0x0040
[ETH] initial PHY link DOWN
[ETH] low_level_init done, waiting for ethernet_link_thread to control link state
[LWIP] Ethernet link thread created
[LWIP] IP: 10.0.0.100
[LWIP] NETMASK: 255.255.255.0
[LWIP] GW: 10.0.0.1
[HEAP] free after LWIP init: 45864
[APP] eth_to_core_queue=0x24004e48 item=64
[APP] core_to_eth_queue=0x240050a0 item=64
[APP] core_to_can_queue=0x240052f8 item=16
[APP] can_to_core_queue=0x240053d0 item=16
[APP] Queues created OK
[CLIENT] task created
[APP] EthApp_Init done
[CLIENT] ClientHandlerTask started
[CORE] task created
[CAN] task created
[CORE] CoreTask started
[MEM] _sbss=0x240000A0 _ebss=0x24018B68
[MEM] lwip_heap: 0x24018B80 .. 0x24028B80
[MEM] LWIP_RAM_HEAP_POINTER=0x24018B80
[ETH] waiting LINK UP (event)... evt=0x24003540 mask=0x00000001
[CORE] eth_to_core_queue=0x24004e48 core_to_eth_queue=0x240050a0
[CORE] core_to_can_queue=0x240052f8 can_to_core_queue=0x240053d0
[ETH] EthernetTask thread created
[CAN] CanTask started
[CAN] core_to_can_queue=0x240052f8 can_to_core_queue=0x240053d0
[ETH] HAL_ETH_Start_IT OK, speed=100M duplex=FULL
[ETH] link_thread: scheduling LINK UP in tcpip_thread
[ETH] netif_link_up_in_tcpip ENTER flags_before=0x1A
[LWIP] netif is DOWN
[LWIP] link is UP
[ETH] tcpip: netif link UP + netif UP
[ETH] tcpip: flags_after=0x1F
[ETH] tcpip: input=0x800ec99 output=0x8017cd5 linkoutput=0x8009b79
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
[TCP] ACCEPT from 10.0.0.2:51250
[TCP] RX chunk len=2
[CORE] got cmd raw: 43 0D
[CORE] parse OK, type=CLOSE
[CORE] channel CLOSED
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[TCP] RX chunk len=3
[CORE] got cmd raw: 53 38 0D
[CORE] parse OK, type=SET_BITRATE
[CORE] parsed bitrate: S8
[CORE] bitrate set: S8
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[TCP] RX chunk len=2
[CORE] got cmd raw: 59 0D
[CORE] parse OK, type=SELF_RECEPTION
[CORE] channel SELF RECEPTION, bitrate=S8
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[TCP] RX chunk len=10
[CORE] got cmd raw: 74 31 32 33 32 31 31 32 32 0D
[CORE] parse OK, type=SEND_FRAME
[CORE] parsed frame: ID=0x00000123 DLC=2 FLAGS=0x00 DATA=11 22
[CORE] frame queued to CAN
[CORE] frame: ID=0x00000123 DLC=2 FLAGS=0x04
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CAN] frame from CORE: DATA ID=0x00000123 DLC=2 FLAGS=0x04 DATA=11 22
[CAN] TX header prepared:
[CAN]   IdType     = STANDARD
[CAN]   Identifier = 0x00000123
[CAN]   FrameType  = DATA
[CAN]   DataLength = 2
[CAN] payload prepared: 11 22
[CAN] TEMP: frame is ready for future HAL_FDCAN_AddMessageToTxFifoQ()
[CAN] DEBUG: frame looped back to can_to_core_queue
[CORE] CAN RX DATA: ID=0x00000123 DLC=2 FLAGS=0x04 DATA=11 22
[CORE] formatted SLCAN: t12321122
[CORE] CAN RX -> ETH OK
[TCP] RX chunk len=15
[CORE] got cmd raw: 54 31 32 33 34 35 36 37 38 32 32 34 33 35 0D
[CORE] parse OK, type=SEND_FRAME
[CORE] parsed frame: ID=0x12345678 DLC=2 FLAGS=0x01 DATA=24 35
[CORE] frame queued to CAN
[CORE] frame: ID=0x12345678 DLC=2 FLAGS=0x05
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CAN] frame from CORE: DATA ID=0x12345678 DLC=2 FLAGS=0x05 DATA=24 35
[CAN] TX header prepared:
[CAN]   IdType     = EXTENDED
[CAN]   Identifier = 0x12345678
[CAN]   FrameType  = DATA
[CAN]   DataLength = 2
[CAN] payload prepared: 24 35
[CAN] TEMP: frame is ready for future HAL_FDCAN_AddMessageToTxFifoQ()
[CAN] DEBUG: frame looped back to can_to_core_queue
[CORE] CAN RX DATA: ID=0x12345678 DLC=2 FLAGS=0x05 DATA=24 35
[CORE] formatted SLCAN: T1234567822435
[CORE] CAN RX -> ETH OK
[TCP] Client disconnected (p==NULL)
[TCP] ACCEPT from 10.0.0.2:51334
[TCP] RX chunk len=7
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
[TCP] Client disconnected (p==NULL)

## Проверка с помощью питоновского скрипта
C:\Users\Egenie\Desktop>py test_loopback_frame.py
Connecting to 10.0.0.100:2001 ...
Connected

--- Close ---
Sending: b'C\r'
Recv: b'\r'

--- Set bitrate S8 ---
Sending: b'S8\r'
Recv: b'\r'

--- Self reception mode ---
Sending: b'Y\r'
Recv: b'\r'

--- Send standard frame ---
Sending: b't12321122\r'
Recv: b'\rt12321122\r'

Expected logic:
1) first should come ACK: b'\r'
2) then loopback frame: b't12321122\r'
Possible combined result:
   b'\rt12321122\r'

--- Send extended frame ---
Sending: b'T1234567822435\r'
Recv: b'\rT1234567822435\r'

Expected logic:
1) ACK: b'\r'
2) loopback frame: b'T1234567822435\r'
Possible combined result:
   b'\rT1234567822435\r'

Socket closed

C:\Users\Egenie\Desktop>py test_combined_packet.py
Connecting to 10.0.0.100:2001 ...
Connected
Sending combined payload: b'C\rS8\rO\r'
Received: b'\r\r\r'
Combined packet test: OK
Socket closed

C:\Users\Egenie\Desktop>
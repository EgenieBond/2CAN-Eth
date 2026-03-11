# 2CAN-Eth - основная ветка

## Версия
Версия проекта от 10 марта

## Ветки
- **main** - стабильная версия
- **Fixing** - ветка для исправлений и тестирования

## Основные настройки
- статический IP 10.0.0.100

## Что реализовано
1. готов и проверен внутренний конвейер Ethernet-модуля:
источник байтов → буферизация → выделение команды → очередь в ядро → обработка → очередь ответа → отправка назад

2. Устройство получает входящие пакеты из сети (что видно из логов через UART)
- ARP пакеты:
[RX] ETH ARP tot=60
[RX] ARP op=1

- IPv4 пакеты:
[RX] ETH IPv4 tot=110
[RX] IPv4 proto=17 src=10.0.0.2

3. Работают очереди между задачами
ClientHandler собирает команду
кладёт её в eth_to_core_queue
CoreTask читает её
парсер формирует ответ
ответ кладётся в core_to_eth_queue
ClientHandler его читает и “отправляет клиенту”

4. CoreTask получает команды и формирует ответы (видно из логов через UART)


## Что пока не реализовано
Для отладки чтение команды по реальному TCP пока не происходит

Настоящий TCP RAW server пока отключён, то есть пока:
- реальный клиент по TCP не подключается
- байты не читаются из TCP-сокета
- ответы не отправляются обратно через tcp_write()/tcp_output()
- Сейчас ClientHandlerTask получает поток байтов из FakeClientSource_Poll(). А в финальной архитектуре он должен получать байты из TCP receive callback / TCP input logic
- Сейчас исходящие ответы уходят только в лог [CLIENT] TX->FAKE_CLIENT:, а не реальному клиенту

## Про модуль ядра
- Уже работает такая цепочка:
FakeClient -> ClientHandler -> eth_to_core_queue -> CoreTask -> Slcan_ParseCommand -> core_to_can_queue -> CanTask(debug)

- валидные команды проходят правильно, невалидные команды отбрасываются правильно
1. Успешные:
S8\r → скорость установлена
O\r → канал открыт
t12321122\r → кадр распарсен, поставлен в core_to_can_queue, считан CanTask
L\r → listen-only включается
C\r → канал закрывается

2. Ошибочные:
t12\r → ошибка парсинга
t123Z122\r → ошибка парсинга
r1239\r → ошибка парсинга из-за DLC > 8
отправка кадра при закрытом канале → ошибка
отправка кадра в listen-only → ошибка


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
[CORE] task created
[CAN] task created
[CORE] CoreTask started
[ETH] Ethernet task started (DebugUART)
[MEM] _sbss=0x240000C8 _ebss=0x24018294
[MEM] lwip_heap: 0x240182A0 .. 0x240282A0
[MEM] LWIP_RAM_HEAP_POINTER=0x240182A0
[ETH] waiting LINK UP (event)... evt=0x24003508 mask=0x00000001
[CORE] eth_to_core_queue=0x24004e10 core_to_eth_queue=0x24005068
[CORE] core_to_can_queue=0x240052c0 can_to_core_queue=0x24005398
[CAN] CanTask started
[CAN] core_to_can_queue=0x240052c0 can_to_core_queue=0x24005398
[CLIENT] ClientHandlerTask started
[FAKE] Fake client source init
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
[CORE] got cmd raw: 53 38 0D
[CORE] parse OK, type=SET_BITRATE
[CORE] parsed bitrate: S8
[CORE] bitrate set: S8
[CORE] resp str:
[CORE] resp raw: 0D
[CLIENT] -> CORE OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 0D
[CLIENT] TX->FAKE_CLIENT:
[CORE] processed OK
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
[CAN] RX from CORE: DATA frame ID=0x00000123 DLC=2 FLAGS=0x00 DATA=11 22
[CAN] TEMP: frame consumed by debug CanTask
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 0D
[CLIENT] TX->FAKE_CLIENT:
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

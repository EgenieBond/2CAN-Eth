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

## Про slcan_parser
- Сейчас он:
принимает строку команды (cmd) -> анализирует её -> записывает ответ в resp
- Какие команды поддерживаются сейчас:
1. O\r — открыть канал
строка сравнивается с "O\r" -> если совпала — возвращается "\r"
2. C\r — закрыть канал
команда распознана -> возвращается OK
3. T... — отправка CAN кадра
Любая команда вида "Txxxxxxxx..." возвращает жёстко заданный ответ: t12341122\r
Это просто имитация ответа, данные не разбираются на части для CAN

## Вывод всего лога через UART:
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
[EVT] g_ethLinkEvt created = 0x240034e0
[EVT] mask APP_ETH_EVT_LINK_UP=0x00000001
[ETH] initial PHY link DOWN
[LWIP] Ethernet link thread created
[LWIP] IP: 10.0.0.100
[HEAP] free after LWIP init: 45968
[HEAP] min ever free:      45968
[LWIP] MX_LWIP_Init done
[APP] eth_to_core_queue=0x24004de8 item=64
[APP] core_to_eth_queue=0x24005040 item=64
[APP] Queues created OK
[CLIENT] task created
[APP] EthApp_Init done
[CLIENT] ClientHandlerTask started
[FAKE] Fake client source init
[CORE] CoreTask started
[CORE] eth_to_core_queue=0x24004de8 core_to_eth_queue=0x24005040
[CORE] task created

[ETH] EthernetTask ENTER (HAL_UART_Transmit)
[ETH] Ethernet task started (DebugUART)
[MEM] _sbss=0x240000A8 _ebss=0x2401826C
[MEM] lwip_heap: 0x24018280 .. 0x24028280
[MEM] LWIP_RAM_HEAP_POINTER=0x24018280
[ETH] waiting LINK UP (event)... evt=0x240034e0 mask=0x00000001
[ETH] EthernetTask thread created
[LWIP] netif is UP
[LWIP] link is UP
[ETH] wait returned=0x00000001 now_get=0x00000000
[ETH] osEventFlagsWait OK: flags=0x00000001
[ETH] LINK UP (task sees it)
[ETH] My IP: 10.0.0.100
[ETH] TEMP: RAW TCP server start is disabled
[CLIENT] complete line: 4F 0D
[CLIENT] eth_to_core_queue=0x24004de8
[CLIENT] SendCmdToCore raw bytes: 4F 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 4F 0D
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 0D
[CLIENT] TX->FAKE_CLIENT:
[CLIENT] complete line: 53 38 0D
[CLIENT] eth_to_core_queue=0x24004de8
[CLIENT] SendCmdToCore raw bytes: 53 38 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 53 38 0D
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 0D
[CLIENT] TX->FAKE_CLIENT:
[CLIENT] complete line: 54 31 32 33 34 31 31 32 32 0D
[CLIENT] eth_to_core_queue=0x24004de8
[CLIENT] SendCmdToCore raw bytes: 54 31 32 33 34 31 31 32 32 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 54 31 32 33 34 31 31 32 32 0D
[CORE] resp str: t12341122
[CORE] resp raw: 74 31 32 33 34 31 31 32 32 0D
[CORE] processed OK
[CLIENT] got resp from CORE: t12341122
[CLIENT] resp raw: 74 31 32 33 34 31 31 32 32 0D
[CLIENT] TX->FAKE_CLIENT: t12341122
[CLIENT] complete line: 43 0D
[CLIENT] eth_to_core_queue=0x24004de8
[CLIENT] SendCmdToCore raw bytes: 43 0D
[CLIENT] osMessageQueuePut -> 0
[CLIENT] -> CORE OK
[CORE] got cmd raw: 43 0D
[CORE] resp str:
[CORE] resp raw: 0D
[CORE] processed OK
[CLIENT] got resp from CORE:
[CLIENT] resp raw: 0D
[CLIENT] TX->FAKE_CLIENT:

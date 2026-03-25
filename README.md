# 2CAN-Eth - ветка для временной копии проекта

## Версия
Версия проекта от 25 марта

## Ветки
- **main** - стабильная версия
- **Fixing** - ветка для исправлений и тестирования всего проекта
- **Fix_TCP_Server** - ветка для исправлений и тестирования части с TCP-сервером

## Текущая задача
- ПК отправляет ARP запросы → но ответ от STM32 на проводе не появляется.
- RX работает. Адреса правильные. ARP-запрос до платы доходит. Ломается именно отправка ARP-ответа обратно с платы.

## В каком месте проблема
- сейчас проблема в обработке ARP внутри стека на плате.
- Цепочка сейчас такая:
1. ПК правильно шлёт ARP request на 10.0.0.100
2. плата этот пакет реально принимает
3. netif->input(p, netif) отрабатывает без ошибки (= 0)
4. но после этого lwIP не инициирует ARP reply
5. поэтому на проводе ответа нет, и ПК пишет «узел недоступен»

## Вывод в путти
[LWIP] Ethernet link thread created
[LWIP] IP: 10.0.0.100
[LWIP] NETMASK: 255.255.255.0
[LWIP] GW: 10.0.0.1
[HEAP] free after LWIP init: 45864
[HEAP] min ever free:      45864
[LWIP] MX_LWIP_Init done

[ETH] EthernetTask ENTER (HAL_UART_Transmit)
[ETH] Ethernet task started (DebugUART)
[MEM] _sbss=0x240000A0 _ebss=0x24018424
[MEM] lwip_heap: 0x24018440 .. 0x24028440
[MEM] LWIP_RAM_HEAP_POINTER=0x24018440
[ETH] waiting LINK UP (event)... evt=0x24003528 mask=0x00000001
[ETH] EthernetTask thread created
[RXALLOC] OK cnt=1 p=0x30004580 buff=0x300045a2 base=0x300045a0 ref=1
[RXALLOC] OK cnt=2 p=0x30003f40 buff=0x30003f62 base=0x30003f60 ref=1
[RXALLOC] OK cnt=3 p=0x30003900 buff=0x30003922 base=0x30003920 ref=1
[RXALLOC] OK cnt=4 p=0x300032c0 buff=0x300032e2 base=0x300032e0 ref=1
[ETH] HAL_ETH_Start_IT OK, speed=100M duplex=FULL
[ETH] link_thread: scheduling LINK UP in tcpip_thread
[LWIP] netif is DOWN
[LWIP] link is UP
[TX] low_level_output ENTER p=0x24018448 tot_len=42 type=0x0806 ref=1
[TX] seg[0]: payload=0x2401845a len=42
[TX] before pbuf_ref: ref=1
[TX] after  pbuf_ref: ref=2
[TX-BEFORE] DMACSR=0x00000000 DMAMR=0x00000000 MACCR=0x3830E003
[TX-BEFORE] MTLTQDR=0x00000000 MTLRQDR=0x00000000
[TX] HAL_ETH_TxCpltCallback ENTER cnt=1 heth=0x24010278 hal_err=0x00000000 dma_err=0x00000000
[TX-CPLT] DMACSR=0x00000404 DMAMR=0x00000000 MACCR=0x3830E003
[TX-CPLT] MTLTQDR=0x00000000 MTLRQDR=0x00000000
[TX] HAL_ETH_TxCpltCallback sem release -> 0
[TX] HAL_ETH_TxCpltCallback EXIT
[TX] HAL_ETH_Transmit_IT ret=0 hal_err=0x00000000 dma_err=0x00000000
[TX-AFTER-CALL] DMACSR=0x00000404 DMAMR=0x00000000 MACCR=0x3830E003
[TX-AFTER-CALL] MTLTQDR=0x00000000 MTLRQDR=0x00000000
[TX] waiting TxPktSemaphore...
[TX] osSemaphoreAcquire(TxPktSemaphore) -> 0
[TX-AFTER-WAIT] DMACSR=0x00000404 DMAMR=0x00000000 MACCR=0x3830E003
[TX-AFTER-WAIT] MTLTQDR=0x00000000 MTLRQDR=0x00000000
[TX] low_level_output EXIT OK ref_now=2
[ETH] tcpip: netif link UP + netif UP
[ETH] tcpip: gratuitous ARP disabled for test
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
[TCP] RawTcpServer_Init enter
[TCP] accept callback installed, pcb=0x2401189c
[TCP] Listening on port 2001
[TCP] init cb: RawTcpServer_Init done
[RXALLOC] OK cnt=5 p=0x30002c80 buff=0x30002ca2 base=0x30002ca0 ref=1
[RXREAD] OK cnt=1 p=0x30004580 len=60 tot_len=60 ref=1 payload=0x300045a2
[RXFREE] cnt=1 p=0x30004580 ref=0 len=60 tot_len=60
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000000 dma_err=0x00000000 DMACSR=0x00000404
[RXALLOC] OK cnt=6 p=0x30004580 buff=0x300045a2 base=0x300045a0 ref=1
[RXREAD] OK cnt=2 p=0x30003f40 len=60 tot_len=60 ref=1 payload=0x30003f62
[RXFREE] cnt=2 p=0x30003f40 ref=0 len=60 tot_len=60
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000000 dma_err=0x00000000 DMACSR=0x00000404
[RXALLOC] OK cnt=7 p=0x30003f40 buff=0x30003f62 base=0x30003f60 ref=1
[RXREAD] OK cnt=3 p=0x30003900 len=60 tot_len=60 ref=1 payload=0x30003922
[RXFREE] cnt=3 p=0x30003900 ref=0 len=60 tot_len=60
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000000 dma_err=0x00000000 DMACSR=0x00000404
[RXALLOC] OK cnt=8 p=0x30003900 buff=0x30003922 base=0x30003920 ref=1
[RXREAD] OK cnt=4 p=0x300032c0 len=60 tot_len=60 ref=1 payload=0x300032e2
[RXFREE] cnt=4 p=0x300032c0 ref=0 len=60 tot_len=60
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000000 dma_err=0x00000000 DMACSR=0x00000404
[ETH] ERROR callback ENTER cnt=1 heth=0x24010278 hal_err=0x00000008 dma_err=0x00004480
[ETH-ERR] DMACSR=0x00000004 DMAMR=0x00000000 MACCR=0x3830E003
[ETH-ERR] MTLTQDR=0x00000000 MTLRQDR=0x00010010
[ETH] ERROR callback tx sem release -> 0
[ETH] ERROR callback: RBU detected -> wake RX
[ETH] ERROR callback rx sem release -> -3
[ETH] ERROR callback EXIT
[RXALLOC] OK cnt=9 p=0x300032c0 buff=0x300032e2 base=0x300032e0 ref=1
[ETH] ERROR callback ENTER cnt=2 heth=0x24010278 hal_err=0x00000008 dma_err=0x00004080
[ETH-ERR] DMACSR=0x00000004 DMAMR=0x00000000 MACCR=0x3830E003
[ETH-ERR] MTLTQDR=0x00000000 MTLRQDR=0x00000000
[ETH] ERROR callback tx sem release -> -3
[ETH] ERROR callback: RBU detected -> wake RX
[ETH] ERROR callback rx sem release -> -3
[ETH] ERROR callback EXIT
[RXREAD] OK cnt=5 p=0x30002c80 len=110 tot_len=110 ref=1 payload=0x30002ca2
[RXFREE] cnt=5 p=0x30002c80 ref=0 len=110 tot_len=110
[RXALLOC] OK cnt=10 p=0x30002c80 buff=0x30002ca2 base=0x30002ca0 ref=1
[RXREAD] OK cnt=6 p=0x30004580 len=110 tot_len=110 ref=1 payload=0x300045a2
[RXFREE] cnt=6 p=0x30004580 ref=0 len=110 tot_len=110
[RXALLOC] OK cnt=11 p=0x30004580 buff=0x300045a2 base=0x300045a0 ref=1
[RXREAD] OK cnt=7 p=0x30003f40 len=110 tot_len=110 ref=1 payload=0x30003f62
[RXFREE] cnt=7 p=0x30003f40 ref=0 len=110 tot_len=110
[RXALLOC] OK cnt=12 p=0x30003f40 buff=0x30003f62 base=0x30003f60 ref=1
[RXREAD] OK cnt=8 p=0x30003900 len=110 tot_len=110 ref=1 payload=0x30003922
[RXFREE] cnt=8 p=0x30003900 ref=0 len=110 tot_len=110
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=13 p=0x30003900 buff=0x30003922 base=0x30003920 ref=1
[RXREAD] OK cnt=9 p=0x300032c0 len=110 tot_len=110 ref=1 payload=0x300032e2
[RXFREE] cnt=9 p=0x300032c0 ref=0 len=110 tot_len=110
[RXALLOC] OK cnt=14 p=0x300032c0 buff=0x300032e2 base=0x300032e0 ref=1
[RXREAD] OK cnt=10 p=0x30002c80 len=110 tot_len=110 ref=1 payload=0x30002ca2
[RXFREE] cnt=10 p=0x30002c80 ref=0 len=110 tot_len=110
[RXALLOC] OK cnt=15 p=0x30002c80 buff=0x30002ca2 base=0x30002ca0 ref=1
[RXREAD] OK cnt=11 p=0x30004580 len=110 tot_len=110 ref=1 payload=0x300045a2
[RXFREE] cnt=11 p=0x30004580 ref=0 len=110 tot_len=110
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=16 p=0x30004580 buff=0x300045a2 base=0x300045a0 ref=1
[RXREAD] OK cnt=12 p=0x30003f40 len=110 tot_len=110 ref=1 payload=0x30003f62
[RXFREE] cnt=12 p=0x30003f40 ref=0 len=110 tot_len=110
[RXALLOC] OK cnt=17 p=0x30003f40 buff=0x30003f62 base=0x30003f60 ref=1
[RXREAD] OK cnt=13 p=0x30003900 len=110 tot_len=110 ref=1 payload=0x30003922
[RXFREE] cnt=13 p=0x30003900 ref=0 len=110 tot_len=110
[RXALLOC] OK cnt=18 p=0x30003900 buff=0x30003922 base=0x30003920 ref=1
[RXREAD] OK cnt=14 p=0x300032c0 len=110 tot_len=110 ref=1 payload=0x300032e2
[RXFREE] cnt=14 p=0x300032c0 ref=0 len=110 tot_len=110
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=19 p=0x300032c0 buff=0x300032e2 base=0x300032e0 ref=1
[RXREAD] OK cnt=15 p=0x30002c80 len=110 tot_len=110 ref=1 payload=0x30002ca2
[RXFREE] cnt=15 p=0x30002c80 ref=0 len=110 tot_len=110
[RXALLOC] OK cnt=20 p=0x30002c80 buff=0x30002ca2 base=0x30002ca0 ref=1
[RXREAD] OK cnt=16 p=0x30004580 len=110 tot_len=110 ref=1 payload=0x300045a2
[RXFREE] cnt=16 p=0x30004580 ref=0 len=110 tot_len=110
[RXALLOC] OK cnt=21 p=0x30004580 buff=0x300045a2 base=0x300045a0 ref=1
[RXREAD] OK cnt=17 p=0x30003f40 len=110 tot_len=110 ref=1 payload=0x30003f62
[RXFREE] cnt=17 p=0x30003f40 ref=0 len=110 tot_len=110
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=22 p=0x30003f40 buff=0x30003f62 base=0x30003f60 ref=1
[RXREAD] OK cnt=18 p=0x30003900 len=60 tot_len=60 ref=1 payload=0x30003922
[RX] ARP request for me from 10.0.0.2
[ARP-DUMP] p=0x30003900 len=60 tot=60 ref=1 payload=0x30003922
[ARP-DUMP] ETH dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ARP-DUMP] htype=1 ptype=0x0800 hlen=6 plen=4 oper=1
[ARP-DUMP] SHA=00:E0:4C:36:0D:1B SPA=10.0.0.2
[ARP-DUMP] THA=00:00:00:00:00:00 TPA=10.0.0.100
[ARP-DUMP] my MAC=00:80:E1:00:00:00 my IP=10.0.0.100
[RXFREE] cnt=18 p=0x30003900 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=23 p=0x30003900 buff=0x30003922 base=0x30003920 ref=1
[RXREAD] OK cnt=19 p=0x300032c0 len=60 tot_len=60 ref=1 payload=0x300032e2
[RX] ARP request for me from 10.0.0.2
[ARP-DUMP] p=0x300032c0 len=60 tot=60 ref=1 payload=0x300032e2
[ARP-DUMP] ETH dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ARP-DUMP] htype=1 ptype=0x0800 hlen=6 plen=4 oper=1
[ARP-DUMP] SHA=00:E0:4C:36:0D:1B SPA=10.0.0.2
[ARP-DUMP] THA=00:00:00:00:00:00 TPA=10.0.0.100
[ARP-DUMP] my MAC=00:80:E1:00:00:00 my IP=10.0.0.100
[RXFREE] cnt=19 p=0x300032c0 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=24 p=0x300032c0 buff=0x300032e2 base=0x300032e0 ref=1
[RXREAD] OK cnt=20 p=0x30002c80 len=60 tot_len=60 ref=1 payload=0x30002ca2
[RX] ARP request for me from 10.0.0.2
[ARP-DUMP] p=0x30002c80 len=60 tot=60 ref=1 payload=0x30002ca2
[ARP-DUMP] ETH dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ARP-DUMP] htype=1 ptype=0x0800 hlen=6 plen=4 oper=1
[ARP-DUMP] SHA=00:E0:4C:36:0D:1B SPA=10.0.0.2
[ARP-DUMP] THA=00:00:00:00:00:00 TPA=10.0.0.100
[ARP-DUMP] my MAC=00:80:E1:00:00:00 my IP=10.0.0.100
[RXFREE] cnt=20 p=0x30002c80 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=25 p=0x30002c80 buff=0x30002ca2 base=0x30002ca0 ref=1
[RXREAD] OK cnt=21 p=0x30004580 len=60 tot_len=60 ref=1 payload=0x300045a2
[RX] ARP request for me from 10.0.0.2
[ARP-DUMP] p=0x30004580 len=60 tot=60 ref=1 payload=0x300045a2
[ARP-DUMP] ETH dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ARP-DUMP] htype=1 ptype=0x0800 hlen=6 plen=4 oper=1
[ARP-DUMP] SHA=00:E0:4C:36:0D:1B SPA=10.0.0.2
[ARP-DUMP] THA=00:00:00:00:00:00 TPA=10.0.0.100
[ARP-DUMP] my MAC=00:80:E1:00:00:00 my IP=10.0.0.100
[RXFREE] cnt=21 p=0x30004580 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=26 p=0x30004580 buff=0x300045a2 base=0x300045a0 ref=1
[RXREAD] OK cnt=22 p=0x30003f40 len=60 tot_len=60 ref=1 payload=0x30003f62
[RX] ARP request for me from 10.0.0.2
[ARP-DUMP] p=0x30003f40 len=60 tot=60 ref=1 payload=0x30003f62
[ARP-DUMP] ETH dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ARP-DUMP] htype=1 ptype=0x0800 hlen=6 plen=4 oper=1
[ARP-DUMP] SHA=00:E0:4C:36:0D:1B SPA=10.0.0.2
[ARP-DUMP] THA=00:00:00:00:00:00 TPA=10.0.0.100
[ARP-DUMP] my MAC=00:80:E1:00:00:00 my IP=10.0.0.100
[RXFREE] cnt=22 p=0x30003f40 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=27 p=0x30003f40 buff=0x30003f62 base=0x30003f60 ref=1
[RXREAD] OK cnt=23 p=0x30003900 len=60 tot_len=60 ref=1 payload=0x30003922
[RX] ARP request for me from 10.0.0.2
[ARP-DUMP] p=0x30003900 len=60 tot=60 ref=1 payload=0x30003922
[ARP-DUMP] ETH dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ARP-DUMP] htype=1 ptype=0x0800 hlen=6 plen=4 oper=1
[ARP-DUMP] SHA=00:E0:4C:36:0D:1B SPA=10.0.0.2
[ARP-DUMP] THA=00:00:00:00:00:00 TPA=10.0.0.100
[ARP-DUMP] my MAC=00:80:E1:00:00:00 my IP=10.0.0.100
[RXFREE] cnt=23 p=0x30003900 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=28 p=0x30003900 buff=0x30003922 base=0x30003920 ref=1
[RXREAD] OK cnt=24 p=0x300032c0 len=60 tot_len=60 ref=1 payload=0x300032e2
[RX] ARP request for me from 10.0.0.2
[ARP-DUMP] p=0x300032c0 len=60 tot=60 ref=1 payload=0x300032e2
[ARP-DUMP] ETH dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ARP-DUMP] htype=1 ptype=0x0800 hlen=6 plen=4 oper=1
[ARP-DUMP] SHA=00:E0:4C:36:0D:1B SPA=10.0.0.2
[ARP-DUMP] THA=00:00:00:00:00:00 TPA=10.0.0.100
[ARP-DUMP] my MAC=00:80:E1:00:00:00 my IP=10.0.0.100
[RXFREE] cnt=24 p=0x300032c0 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=29 p=0x300032c0 buff=0x300032e2 base=0x300032e0 ref=1
[RXREAD] OK cnt=25 p=0x30002c80 len=60 tot_len=60 ref=1 payload=0x30002ca2
[RX] ARP request for me from 10.0.0.2
[ARP-DUMP] p=0x30002c80 len=60 tot=60 ref=1 payload=0x30002ca2
[ARP-DUMP] ETH dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ARP-DUMP] htype=1 ptype=0x0800 hlen=6 plen=4 oper=1
[ARP-DUMP] SHA=00:E0:4C:36:0D:1B SPA=10.0.0.2
[ARP-DUMP] THA=00:00:00:00:00:00 TPA=10.0.0.100
[ARP-DUMP] my MAC=00:80:E1:00:00:00 my IP=10.0.0.100
[RXFREE] cnt=25 p=0x30002c80 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=30 p=0x30002c80 buff=0x30002ca2 base=0x30002ca0 ref=1
[RXREAD] OK cnt=26 p=0x30004580 len=60 tot_len=60 ref=1 payload=0x300045a2
[RX] ARP request for me from 10.0.0.2
[ARP-DUMP] p=0x30004580 len=60 tot=60 ref=1 payload=0x300045a2
[ARP-DUMP] ETH dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ARP-DUMP] htype=1 ptype=0x0800 hlen=6 plen=4 oper=1
[ARP-DUMP] SHA=00:E0:4C:36:0D:1B SPA=10.0.0.2
[ARP-DUMP] THA=00:00:00:00:00:00 TPA=10.0.0.100
[ARP-DUMP] my MAC=00:80:E1:00:00:00 my IP=10.0.0.100
[RXFREE] cnt=26 p=0x30004580 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=31 p=0x30004580 buff=0x300045a2 base=0x300045a0 ref=1
[RXREAD] OK cnt=27 p=0x30003f40 len=60 tot_len=60 ref=1 payload=0x30003f62
[RX] ARP request for me from 10.0.0.2
[ARP-DUMP] p=0x30003f40 len=60 tot=60 ref=1 payload=0x30003f62
[ARP-DUMP] ETH dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ARP-DUMP] htype=1 ptype=0x0800 hlen=6 plen=4 oper=1
[ARP-DUMP] SHA=00:E0:4C:36:0D:1B SPA=10.0.0.2
[ARP-DUMP] THA=00:00:00:00:00:00 TPA=10.0.0.100
[ARP-DUMP] my MAC=00:80:E1:00:00:00 my IP=10.0.0.100
[RXFREE] cnt=27 p=0x30003f40 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=32 p=0x30003f40 buff=0x30003f62 base=0x30003f60 ref=1
[RXREAD] OK cnt=28 p=0x30003900 len=60 tot_len=60 ref=1 payload=0x30003922
[RX] ARP request for me from 10.0.0.2
[RXFREE] cnt=28 p=0x30003900 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004
[RXALLOC] OK cnt=33 p=0x30003900 buff=0x30003922 base=0x30003920 ref=1
[RXREAD] OK cnt=29 p=0x300032c0 len=60 tot_len=60 ref=1 payload=0x300032e2
[RX] ARP request for me from 10.0.0.2
[RXFREE] cnt=29 p=0x300032c0 ref=0 len=60 tot_len=60
[RX->LWIP] ARP-for-me netif->input = 0
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000008 dma_err=0x00004080 DMACSR=0x00000004


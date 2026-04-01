# 2CAN-Eth - ветка для временной копии проекта

## Версия
Версия проекта от 1 апреля

## Ветки
- **main** - стабильная версия
- **Fixing** - ветка для исправлений и тестирования всего проекта
- **Fix_TCP_Server** - ветка для исправлений и тестирования части с TCP-сервером

## Текущая задача
- ПК отправляет ARP запросы → но ответ от STM32 на проводе не появляется.
- RX работает. Адреса правильные. ARP-запрос до платы доходит. Ломается именно отправка ARP-ответа обратно с платы.

## В каком месте проблема
- плата видит ARP request от ПК и понимает, что запрос адресован ей
- На уровне lwIP всё уже нормально, она формирует ответ:
	srcMAC=00:80:E1:00:00:00
	dstMAC=00:E0:4C:36:0D:1B
	srcIP=10.0.0.100
	dstIP=10.0.0.2
- Не проходит физическая передача ARP-ответа через HAL_ETH_Transmit_IT / DMA TX
	[TXPATH] HAL_ETH_Transmit_IT ret=1 hal_err=0x0000000A dma_err=0x00004080

То есть:
lwIP вызывает отправку -> кадр формируется -> в low_level_output() заходит -> но драйвер ETH отказывает на этапе реальной передачи

## Вывод в путти
[LWIP] link is UP
[ETHARP-RAW] ENTER opcode=1 netif=0x24010204
[ETHARP-RAW] ethsrc=00:80:E1:00:00:00 ethdst=FF:FF:FF:FF:FF:FF
[ETHARP-RAW] hwsrc=00:80:E1:00:00:00 hwdst=00:00:00:00:00:00
[ETHARP-RAW] ipsrc=10.0.0.100 ipdst=10.0.0.100
[ETHARP-RAW] pbuf_alloc OK p=0x240184c8 len=28 tot_len=28 payload=0x240184e8
[ETHARP-RAW] calling ethernet_output p=0x240184c8
[TXPATH] low_level_output #1 p=0x240184c8 tot_len=42 type=0x0806 ref=1
[TXPATH] dst=FF:FF:FF:FF:FF:FF src=00:80:E1:00:00:00
[TX] HAL_ETH_TxCpltCallback ENTER cnt=1 heth=0x2401027c hal_err=0x00000000 dma_err=0x00000000
[TX-CPLT] DMACSR=0x00000404 DMAMR=0x00000000 MACCR=0x3830E003
[TX-CPLT] MTLTQDR=0x00000000 MTLRQDR=0x00000000
[TX] HAL_ETH_TxCpltCallback sem release -> 0
[TX] HAL_ETH_TxCpltCallback EXIT
[TXPATH] HAL_ETH_Transmit_IT ret=0 hal_err=0x00000000 dma_err=0x00000000
[ETHARP-RAW] ethernet_output result=0
[ETHARP-RAW] freeing tx pbuf=0x240184c8
[ETHARP-RAW] EXIT result=0
[ETH] tcpip: netif link UP + netif UP
[ETH] tcpip: flags_after=0x1F
[ETH] tcpip: input=0x800e291 output=0x8016b8d linkoutput=0x8008f09
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
[TCP] RawTcpServer_Init enter
[TCP] accept callback installed, pcb=0x24011930
[TCP] Listening on port 2001
[TCP] init cb: RawTcpServer_Init done
[RXALLOC] OK cnt=5 pcustom=0x30002c80 pbuf=0x30002c80 buff=0x30002ca2 base=0x30002ca0 payload=0x30002ca0
[RXREAD] OK cnt=1 p=0x30004580 len=60 tot_len=60 ref=1 payload=0x300045a0
[RX->INPUT] p=0x30004580 len=60 tot=60 ref=1 input_fn=0x800e291 arp_for_me=0
[PKT-DUMP] p=0x30004580 len=60 tot=60 ref=1 payload=0x300045a0 eth_type=0x0D1B
[PKT-DUMP] ETH dst=00:00:FF:FF:FF:FF src=FF:FF:00:E0:4C:36
[PKT-DUMP] non-ARP/non-IPv4 eth_type=0x0D1B
[ETH-IN] sizeof(struct eth_addr)=6 sizeof(struct eth_hdr)=16
[ETH-IN] ENTER p=0x30004580 len=60 tot=60 ref=1 payload=0x300045a0 netif=0x24010204 flags=0x1F
[ETH-IN] ETH hdr dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ETH-IN] mark LLBCAST
[ETH-IN] CASE ETHTYPE_ARP flags=0x1F
[ETH-IN] ARP -> etharp_input p=0x30004580 payload=0x300045b0 len=44 tot=44
[ETHARP] input ENTER p=0x30004580 len=44 tot=44 payload=0x300045b0 netif=0x24010204
[ETHARP] hdr raw: hwtype=0x0001 proto=0x0800 hwlen=6 protolen=4 opcode=0x0001
[ETHARP] parsed: SIP=0.0.0.0 DIP=10.0.0.2 myIP=10.0.0.100
[ETHARP] shwaddr=00:E0:4C:36:0D:1B dhwaddr=00:00:00:00:00:00
[ETHARP] for_us=0
[ETHARP] update ARP cache: for_us=0
[ETHARP] opcode=ARP_REQUEST
[ETHARP] ARP request not for us
[ETHARP] freeing incoming ARP pbuf=0x30004580
[RXFREE] cnt=1 p=0x30004580 ref=0 len=44 tot_len=44
[ETHARP] input EXIT
[ETH-IN] returned from etharp_input
[ETH-IN] EXIT ERR_OK (packet consumed)
[RX->INPUT] netif->input returned 0, tx_before=1 tx_after=1
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000000 dma_err=0x00000000 DMACSR=0x00000404
[RXALLOC] OK cnt=6 pcustom=0x30004580 pbuf=0x30004580 buff=0x300045a2 base=0x300045a0 payload=0x300045a0
[RXREAD] OK cnt=2 p=0x30003f40 len=60 tot_len=60 ref=1 payload=0x30003f60
[RX->INPUT] p=0x30003f40 len=60 tot=60 ref=1 input_fn=0x800e291 arp_for_me=0
[PKT-DUMP] p=0x30003f40 len=60 tot=60 ref=1 payload=0x30003f60 eth_type=0x0D1B
[PKT-DUMP] ETH dst=00:00:FF:FF:FF:FF src=FF:FF:00:E0:4C:36
[PKT-DUMP] non-ARP/non-IPv4 eth_type=0x0D1B
[ETH-IN] sizeof(struct eth_addr)=6 sizeof(struct eth_hdr)=16
[ETH-IN] ENTER p=0x30003f40 len=60 tot=60 ref=1 payload=0x30003f60 netif=0x24010204 flags=0x1F
[ETH-IN] ETH hdr dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ETH-IN] mark LLBCAST
[ETH-IN] CASE ETHTYPE_ARP flags=0x1F
[ETH-IN] ARP -> etharp_input p=0x30003f40 payload=0x30003f70 len=44 tot=44
[ETHARP] input ENTER p=0x30003f40 len=44 tot=44 payload=0x30003f70 netif=0x24010204
[ETHARP] hdr raw: hwtype=0x0001 proto=0x0800 hwlen=6 protolen=4 opcode=0x0001
[ETHARP] parsed: SIP=0.0.0.0 DIP=10.0.0.2 myIP=10.0.0.100
[ETHARP] shwaddr=00:E0:4C:36:0D:1B dhwaddr=00:00:00:00:00:00
[ETHARP] for_us=0
[ETHARP] update ARP cache: for_us=0
[ETHARP] opcode=ARP_REQUEST
[ETHARP] ARP request not for us
[ETHARP] freeing incoming ARP pbuf=0x30003f40
[RXFREE] cnt=2 p=0x30003f40 ref=0 len=44 tot_len=44
[ETHARP] input EXIT
[ETH-IN] returned from etharp_input
[ETH-IN] EXIT ERR_OK (packet consumed)
[RX->INPUT] netif->input returned 0, tx_before=1 tx_after=1
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000000 dma_err=0x00000000 DMACSR=0x00000404
[RXALLOC] OK cnt=7 pcustom=0x30003f40 pbuf=0x30003f40 buff=0x30003f62 base=0x30003f60 payload=0x30003f60
[RXREAD] OK cnt=3 p=0x30003900 len=60 tot_len=60 ref=1 payload=0x30003920
[RX->INPUT] p=0x30003900 len=60 tot=60 ref=1 input_fn=0x800e291 arp_for_me=0
[PKT-DUMP] p=0x30003900 len=60 tot=60 ref=1 payload=0x30003920 eth_type=0x0D1B
[PKT-DUMP] ETH dst=00:00:FF:FF:FF:FF src=FF:FF:00:E0:4C:36
[PKT-DUMP] non-ARP/non-IPv4 eth_type=0x0D1B
[ETH-IN] sizeof(struct eth_addr)=6 sizeof(struct eth_hdr)=16
[ETH-IN] ENTER p=0x30003900 len=60 tot=60 ref=1 payload=0x30003920 netif=0x24010204 flags=0x1F
[ETH-IN] ETH hdr dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ETH-IN] mark LLBCAST
[ETH-IN] CASE ETHTYPE_ARP flags=0x1F
[ETH-IN] ARP -> etharp_input p=0x30003900 payload=0x30003930 len=44 tot=44
[ETHARP] input ENTER p=0x30003900 len=44 tot=44 payload=0x30003930 netif=0x24010204
[ETHARP] hdr raw: hwtype=0x0001 proto=0x0800 hwlen=6 protolen=4 opcode=0x0001
[ETHARP] parsed: SIP=0.0.0.0 DIP=10.0.0.2 myIP=10.0.0.100
[ETHARP] shwaddr=00:E0:4C:36:0D:1B dhwaddr=00:00:00:00:00:00
[ETHARP] for_us=0
[ETHARP] update ARP cache: for_us=0
[ETHARP] opcode=ARP_REQUEST
[ETHARP] ARP request not for us
[ETHARP] freeing incoming ARP pbuf=0x30003900
[RXFREE] cnt=3 p=0x30003900 ref=0 len=44 tot_len=44
[ETHARP] input EXIT
[ETH-IN] returned from etharp_input
[ETH-IN] EXIT ERR_OK (packet consumed)
[RX->INPUT] netif->input returned 0, tx_before=1 tx_after=1
[RXREAD] HAL_ETH_ReadData FAIL st=1 hal_err=0x00000000 dma_err=0x00000000 DMACSR=0x00000404
[RXALLOC] OK cnt=8 pcustom=0x30003900 pbuf=0x30003900 buff=0x30003922 base=0x30003920 payload=0x30003920
[RXREAD] OK cnt=4 p=0x300032c0 len=60 tot_len=60 ref=1 payload=0x300032e0
[RX->INPUT] p=0x300032c0 len=60 tot=60 ref=1 input_fn=0x800e291 arp_for_me=0
[PKT-DUMP] p=0x300032c0 len=60 tot=60 ref=1 payload=0x300032e0 eth_type=0x0D1B
[PKT-DUMP] ETH dst=00:00:FF:FF:FF:FF src=FF:FF:00:E0:4C:36
[PKT-DUMP] non-ARP/non-IPv4 eth_type=0x0D1B
[ETH-IN] sizeof(struct eth_addr)=6 sizeof(struct eth_hdr)=16
[ETH-IN] ENTER p=0x300032c0 len=60 tot=60 ref=1 payload=0x300032e0 netif=0x24010204 flags=0x1F
[ETH-IN] ETH hdr dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0806
[ETH-IN] mark LLBCAST
[ETH-IN] CASE ETHTYPE_ARP flags=0x1F
[ETH-IN] ARP -> etharp_input p=0x300032c0 payload=0x300032f0 len=44 tot=44
[ETHARP] input ENTER p=0x300032c0 len=44 tot=44 payload=0x300032f0 netif=0x24010204
[ETHARP] hdr raw: hwtype=0x0001 proto=0x0800 hwlen=6 protolen=4 opcode=0x0001
[ETHARP] parsed: SIP=10.0.0.2 DIP=10.0.0.2 myIP=10.0.0.100
[ETHARP] shwaddr=00:E0:4C:36:0D:1B dhwaddr=00:00:00:00:00:00
[ETHARP] for_us=0
[ETHARP] update ARP cache: for_us=0
[ETHARP] opcode=ARP_REQUEST
[ETHARP] ARP request not for us
[ETHARP] freeing incoming ARP pbuf=0x300032c0
[RXFREE] cnt=4 p=0x300032c0 ref=0 len=44 tot_len=44
[ETHARP] input EXIT
[ETH-IN] returned from etharp_input
[ETH-IN] EXIT ERR_OK (packet consumed)
[RX->INPUT] netif->input returned 0, tx_before=1 tx_after=1
[RXALLOC] OK cnt=9 pcustom=0x300032c0 pbuf=0x300032c0 buff=0x300032e2 base=0x300032e0 payload=0x300032e0
[RXREAD] OK cnt=5 p=0x30002c80 len=110 tot_len=110 ref=1 payload=0x30002ca0
[RX->INPUT] p=0x30002c80 len=110 tot=110 ref=1 input_fn=0x800e291 arp_for_me=0
[PKT-DUMP] p=0x30002c80 len=110 tot=110 ref=1 payload=0x30002ca0 eth_type=0x0D1B
[PKT-DUMP] ETH dst=00:00:FF:FF:FF:FF src=FF:FF:00:E0:4C:36
[PKT-DUMP] non-ARP/non-IPv4 eth_type=0x0D1B
[ETH-IN] sizeof(struct eth_addr)=6 sizeof(struct eth_hdr)=16
[ETH-IN] ENTER p=0x30002c80 len=110 tot=110 ref=1 payload=0x30002ca0 netif=0x24010204 flags=0x1F
[ETH-IN] ETH hdr dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0800
[ETH-IN] mark LLBCAST
[ETH-IN] CASE ETHTYPE_IP flags=0x1F
[ETH-IN] IPv4 -> ip4_input p=0x30002c80 payload=0x30002cb0 len=94 tot=94
[RXFREE] cnt=5 p=0x30002c80 ref=0 len=94 tot_len=94
[ETH-IN] EXIT ERR_OK (packet consumed)
[RX->INPUT] netif->input returned 0, tx_before=1 tx_after=1
[RXALLOC] OK cnt=10 pcustom=0x30002c80 pbuf=0x30002c80 buff=0x30002ca2 base=0x30002ca0 payload=0x30002ca0
[RXREAD] OK cnt=6 p=0x30004580 len=110 tot_len=110 ref=1 payload=0x300045a0
[RX->INPUT] p=0x30004580 len=110 tot=110 ref=1 input_fn=0x800e291 arp_for_me=0
[PKT-DUMP] p=0x30004580 len=110 tot=110 ref=1 payload=0x300045a0 eth_type=0x0D1B
[PKT-DUMP] ETH dst=00:00:FF:FF:FF:FF src=FF:FF:00:E0:4C:36
[PKT-DUMP] non-ARP/non-IPv4 eth_type=0x0D1B
[ETH-IN] sizeof(struct eth_addr)=6 sizeof(struct eth_hdr)=16
[ETH-IN] ENTER p=0x30004580 len=110 tot=110 ref=1 payload=0x300045a0 netif=0x24010204 flags=0x1F
[ETH-IN] ETH hdr dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0800
[ETH-IN] mark LLBCAST
[ETH-IN] CASE ETHTYPE_IP flags=0x1F
[ETH-IN] IPv4 -> ip4_input p=0x30004580 payload=0x300045b0 len=94 tot=94
[RXFREE] cnt=6 p=0x30004580 ref=0 len=94 tot_len=94
[ETH-IN] EXIT ERR_OK (packet consumed)
[RX->INPUT] netif->input returned 0, tx_before=1 tx_after=1
[RXALLOC] OK cnt=11 pcustom=0x30004580 pbuf=0x30004580 buff=0x300045a2 base=0x300045a0 payload=0x300045a0
[RXREAD] OK cnt=7 p=0x30003f40 len=110 tot_len=110 ref=1 payload=0x30003f60
[RX->INPUT] p=0x30003f40 len=110 tot=110 ref=1 input_fn=0x800e291 arp_for_me=0
[PKT-DUMP] p=0x30003f40 len=110 tot=110 ref=1 payload=0x30003f60 eth_type=0x0D1B
[PKT-DUMP] ETH dst=00:00:FF:FF:FF:FF src=FF:FF:00:E0:4C:36
[PKT-DUMP] non-ARP/non-IPv4 eth_type=0x0D1B
[ETH-IN] sizeof(struct eth_addr)=6 sizeof(struct eth_hdr)=16
[ETH-IN] ENTER p=0x30003f40 len=110 tot=110 ref=1 payload=0x30003f60 netif=0x24010204 flags=0x1F
[ETH-IN] ETH hdr dst=FF:FF:FF:FF:FF:FF src=00:E0:4C:36:0D:1B type=0x0800
[ETH-IN] mark LLBCAST
[ETH-IN] CASE ETHTYPE_IP flags=0x1F
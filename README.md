# 2CAN-Eth - ветка для временной копии проекта

## Версия
Версия проекта от 31 апреля

## Ветки
- **main** - стабильная версия
- **Fixing** - ветка для исправлений и тестирования всего проекта
- **Fix_TCP_Server** - ветка для исправлений и тестирования части с TCP-сервером

## Текущая задача
- ПК отправляет ARP запросы → но ответ от STM32 на проводе не появляется.
- RX работает. Адреса правильные. ARP-запрос до платы доходит. Ломается именно отправка ARP-ответа обратно с платы.

## В каком месте проблема
- ARP работает
- Wireshark видит reply
- arp -a показывает 10.0.0.100 -> 00-80-e1-00-00-00
- ICMP Echo Request до платы доходит

в логе есть:
[RX] IPv4 ICMP type=8 code=0 ...

Но ICMP Echo Reply назад не уходит, в логе нет
[TX] IPv4 ICMP type=0 code=0 ...

То есть Ethernet и ARP мы уже подняли.

## Вывод в путти
C:\Windows\system32>ping 10.0.0.100

Обмен пакетами с 10.0.0.100 по с 32 байтами данных:
Превышен интервал ожидания для запроса.
Превышен интервал ожидания для запроса.
Превышен интервал ожидания для запроса.
Превышен интервал ожидания для запроса.

Статистика Ping для 10.0.0.100:
    Пакетов: отправлено = 4, получено = 0, потеряно = 4
    (100% потерь)

C:\Windows\system32>arp -a

Интерфейс: 10.0.0.2 --- 0x8
  адрес в Интернете      Физический адрес      Тип
  10.0.0.100            00-80-e1-00-00-00     динамический
  10.0.0.255            ff-ff-ff-ff-ff-ff     статический
  224.0.0.22            01-00-5e-00-00-16     статический
  224.0.0.251           01-00-5e-00-00-fb     статический
  224.0.0.252           01-00-5e-00-00-fc     статический
  239.255.255.250       01-00-5e-7f-ff-fa     статический

C:\Windows\system32>

[ETH] tcpip: netif link UP + netif UP
[ETH] tcpip: flags_after=0x1F
[ETH] tcpip: input=0x800e551 output=0x8016b09 linkoutput=0x80092c5
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
[TCP] accept callback installed, pcb=0x24011f3c
[TCP] Listening on port 2001
[TCP] init cb: RawTcpServer_Init done
[ETH-STAT] RX_CPLT: eth_irq=2 rx_irq=1 rx_sem=0 rx_ok=0 rx_fail=0 rx_in_err=0 alloc_ok=8 alloc_fail=0 arp_for_me=0 tx_call=1 tx_ok=1 tx_fail=0 tx_cplt=1 tx_err=0 tx_timeout=0
[RX] ARP oper=1 sip=0.0.0.0 tip=10.0.0.2 my=10.0.0.100
[ETH-STAT] RX_OK: eth_irq=2 rx_irq=1 rx_sem=1 rx_ok=1 rx_fail=0 rx_in_err=0 alloc_ok=9 alloc_fail=0 arp_for_me=0 tx_call=1 tx_ok=1 tx_fail=0 tx_cplt=1 tx_err=0 tx_timeout=0
[ETH-STAT] RX_FAIL: eth_irq=2 rx_irq=1 rx_sem=1 rx_ok=1 rx_fail=1 rx_in_err=0 alloc_ok=9 alloc_fail=0 arp_for_me=0 tx_call=1 tx_ok=1 tx_fail=0 tx_cplt=1 tx_err=0 tx_timeout=0
[ETH-STAT] RX_CPLT: eth_irq=3 rx_irq=2 rx_sem=1 rx_ok=1 rx_fail=1 rx_in_err=0 alloc_ok=9 alloc_fail=0 arp_for_me=0 tx_call=1 tx_ok=1 tx_fail=0 tx_cplt=1 tx_err=0 tx_timeout=0
[RX] ARP oper=1 sip=0.0.0.0 tip=10.0.0.2 my=10.0.0.100
[ETH-STAT] RX_OK: eth_irq=3 rx_irq=2 rx_sem=2 rx_ok=2 rx_fail=1 rx_in_err=0 alloc_ok=10 alloc_fail=0 arp_for_me=0 tx_call=1 tx_ok=1 tx_fail=0 tx_cplt=1 tx_err=0 tx_timeout=0
[ETH-STAT] RX_FAIL: eth_irq=3 rx_irq=2 rx_sem=2 rx_ok=2 rx_fail=2 rx_in_err=0 alloc_ok=10 alloc_fail=0 arp_for_me=0 tx_call=1 tx_ok=1 tx_fail=0 tx_cplt=1 tx_err=0 tx_timeout=0
[ETH-STAT] RX_CPLT: eth_irq=4 rx_irq=3 rx_sem=2 rx_ok=2 rx_fail=2 rx_in_err=0 alloc_ok=10 alloc_fail=0 arp_for_me=0 tx_call=1 tx_ok=1 tx_fail=0 tx_cplt=1 tx_err=0 tx_timeout=0
[RX] ARP oper=1 sip=0.0.0.0 tip=10.0.0.2 my=10.0.0.100
[ETH-STAT] RX_OK: eth_irq=4 rx_irq=3 rx_sem=3 rx_ok=3 rx_fail=2 rx_in_err=0 alloc_ok=11 alloc_fail=0 arp_for_me=0 tx_call=1 tx_ok=1 tx_fail=0 tx_cplt=1 tx_err=0 tx_timeout=0
[ETH-STAT] RX_FAIL: eth_irq=4 rx_irq=3 rx_sem=3 rx_ok=3 rx_fail=3 rx_in_err=0 alloc_ok=11 alloc_fail=0 arp_for_me=0 tx_call=1 tx_ok=1 tx_fail=0 tx_cplt=1 tx_err=0 tx_timeout=0
[RX] ARP oper=1 sip=10.0.0.2 tip=10.0.0.2 my=10.0.0.100
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] ARP oper=1 sip=10.0.0.2 tip=169.254.169.254 my=10.0.0.100
[RX] ARP oper=1 sip=10.0.0.2 tip=169.254.169.254 my=10.0.0.100
[RX] ARP oper=1 sip=10.0.0.2 tip=169.254.169.254 my=10.0.0.100
[RX] ARP oper=1 sip=10.0.0.2 tip=10.0.0.100 my=10.0.0.100
[ETH-STAT] ARP_FOR_ME: eth_irq=22 rx_irq=21 rx_sem=16 rx_ok=21 rx_fail=15 rx_in_err=0 alloc_ok=29 alloc_fail=0 arp_for_me=1 tx_call=1 tx_ok=1 tx_fail=0 tx_cplt=1 tx_err=0 tx_timeout=0
[TX] ETH dst=00:E0:4C:36:0D:1B src=00:80:E1:00:00:00 type=0x0806 len=42
[TX] ARP oper=2 sha=00:80:E1:00:00:00 spa=10.0.0.100 tha=00:E0:4C:36:0D:1B tpa=10.0.0.2
[ETH-STAT] TX_CPLT: eth_irq=23 rx_irq=21 rx_sem=16 rx_ok=21 rx_fail=15 rx_in_err=0 alloc_ok=29 alloc_fail=0 arp_for_me=1 tx_call=2 tx_ok=2 tx_fail=0 tx_cplt=2 tx_err=0 tx_timeout=0
[RX] IPv4 ICMP type=8 code=0 id=0x0001 seq=4053 src=10.0.0.2 dst=10.0.0.100
[RX] IPv4 ICMP type=8 code=0 id=0x0001 seq=4054 src=10.0.0.2 dst=10.0.0.100
[RX] IPv4 ICMP type=8 code=0 id=0x0001 seq=4055 src=10.0.0.2 dst=10.0.0.100
[RX] IPv4 ICMP type=8 code=0 id=0x0001 seq=4056 src=10.0.0.2 dst=10.0.0.100
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
[RX] IPv4 proto=17 src=10.0.0.2 dst=10.0.0.255
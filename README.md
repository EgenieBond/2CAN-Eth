# 2CAN-Eth - ветка для временной копии проекта
## Ветки
- **main** - стабильная версия
- **Fixing** - ветка для исправлений и тестирования всего проекта
- **Test_Simple_TCP** - ветка для тестирования скорости работы Ethernet

## Последняя версия (25.08)
Скорость почти не меняется, успешно передано 1024МБ по TCP, стабильная скорость около 30 Mbit/s

## Логи со стороны клиента
    10 МБ отправлено |    2.7 с | 30.69 Mbit/s
    20 МБ отправлено |    5.6 с | 30.11 Mbit/s
    30 МБ отправлено |    8.4 с | 30.14 Mbit/s
    40 МБ отправлено |   11.2 с | 29.92 Mbit/s
    50 МБ отправлено |   14.0 с | 29.92 Mbit/s
    60 МБ отправлено |   16.8 с | 29.88 Mbit/s
    70 МБ отправлено |   19.6 с | 29.91 Mbit/s
...
   950 МБ отправлено |  260.7 с | 30.57 Mbit/s
   960 МБ отправлено |  263.4 с | 30.58 Mbit/s
   970 МБ отправлено |  266.1 с | 30.58 Mbit/s
   980 МБ отправлено |  268.8 с | 30.58 Mbit/s
   990 МБ отправлено |  271.6 с | 30.58 Mbit/s
  1000 МБ отправлено |  274.3 с | 30.59 Mbit/s
  1010 МБ отправлено |  276.9 с | 30.60 Mbit/s
  1020 МБ отправлено |  279.7 с | 30.60 Mbit/s

## Логи со стороны платы
[TCP] ACCEPT from 192.168.0.1:62350
[BENCH RX] === START === tick=34224 ms
[BENCH] 10 MB | 890 ms | 94266 Kbit/s (94 Mbit/s) | rcv_wnd=17520 rcv_nxt=1439999247
[ETH-STAT] BENCH: eth_irq=0 rx_irq=7213 rx_sem=8451 rx_ok=7212 rx_fail=7211 rx_in_err=0 alloc_ok=7215 alloc_fail=1239 arp_for_me=0 tx_call=3595 tx_ok=3595 tx_fail=0 tx_cplt=3595 tx_err=0 tx_timeout=0 dma_err_cnt=0 last_dma_err=0x00000000 last_hal_err=0x00[BENCH] 20 MB | 1810 ms | 92704 Kbit/s (92 Mbit/s) | rcv_wnd=17520 rcv_nxt=1450486427
[ETH-STAT] BENCH: eth_irq=0 rx_irq=14396 rx_sem=16810 rx_ok=14395 rx_fail=14393 rx_in_err=0 alloc_ok=14398 alloc_fail=2415 arp_for_me=0 tx_call=7187 tx_ok=7187 tx_fail=0 tx_cplt=7187 tx_err=0 tx_timeout=0 dma_err_cnt=0 last_dma_err=0x00000000 last_hal_err[BENCH] 30 MB | 2712 ms | 92806 Kbit/s (92 Mbit/s) | rcv_wnd=17520 rcv_nxt=1460973607
[ETH-STAT] BENCH: eth_irq=0 rx_irq=21580 rx_sem=25227 rx_ok=21578 rx_fail=21575 rx_in_err=0 alloc_ok=21581 alloc_fail=3648 arp_for_me=0 tx_call=10780 tx_ok=10780 tx_fail=0 tx_cplt=10780 tx_err=1 tx_timeout=0 dma_err_cnt=1 last_dma_err=0x00004480 last_hal_[BENCH] 40 MB | 3640 ms | 92194 Kbit/s (92 Mbit/s) | rcv_wnd=17520 rcv_nxt=1471460787
...
[ETH-STAT] BENCH: eth_irq=0 rx_irq=718335 rx_sem=810479 rx_ok=718333 rx_fail=716064 rx_in_err=0 alloc_ok=718336 alloc_fail=94215 arp_for_me=0 tx_call=401419 tx_ok=317017 tx_fail=84402 tx_cplt=317017 tx_err=1202 tx_timeout=0 dma_err_cnt=1202 last_dma_err=0[BENCH] 1010 MB | 89743 ms | 94421 Kbit/s (94 Mbit/s) | rcv_wnd=17520 rcv_nxt=2488717081
[ETH-STAT] BENCH: eth_irq=0 rx_irq=725518 rx_sem=818362 rx_ok=725516 rx_fail=723223 rx_in_err=0 alloc_ok=725519 alloc_fail=94937 arp_for_me=0 tx_call=405704 tx_ok=319917 tx_fail=85787 tx_cplt=319917 tx_err=1215 tx_timeout=0 dma_err_cnt=1215 last_dma_err=0[BENCH] 1020 MB | 90630 ms | 94422 Kbit/s (94 Mbit/s) | rcv_wnd=17520 rcv_nxt=2499204261
[ETH-STAT] BENCH: eth_irq=0 rx_irq=732697 rx_sem=826266 rx_ok=732697 rx_fail=730366 rx_in_err=0 alloc_ok=732701 alloc_fail=95698 arp_for_me=0 tx_call=409976 tx_ok=322827 tx_fail=87149 tx_cplt=322827 tx_err=1238 tx_timeout=0 dma_err_cnt=1238 last_dma_err=0[BENCH RX] 
=== DONE ===
[BENCH RX] start tick : 34224 ms
[BENCH RX] end tick   : 125192 ms
[BENCH RX] duration   : 90968 ms
[BENCH RX] total bytes: 1073741824
[BENCH RX] avg speed  : 94428 Kbit/s (94 Mbit/s)
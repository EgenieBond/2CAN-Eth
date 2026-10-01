"""Тест производительности передачи данных между двумя устройствами NetCAN."""
import random
import time
import can  # python-can
import vs_can_lib
from contextlib import ExitStack
from types import MappingProxyType
from vs_can_lib import VSCAN, VSCAN_SPEED_1M, VSCAN_SPEED_125K, VSCAN_SPEED_250K, VSCAN_SPEED_500K, \
    VSCAN_MODE_SELF_RECEPTION
# ============================================================
#                        НАСТРОЙКИ
# ============================================================
NETCAN_ADDR_1 = "192.168.0.17:2001"
NETCAN_ADDR_2 = "192.168.0.18:2001"
CANABLE_PORT = "COM8"                  # COM-порт адаптера CANable

MSG_DATA_SIZE = 8          # размер данных в одном CAN-кадре, байт
TEST_CAN_ID = 0x10203040   # ID, с которым отправляются тестовые кадры
MSG_COUNT = 1              # сколько кадров отправлять за один проход по умолчанию

# Интервал (в секундах) между отправками в режиме цикла (repeat=True).
REPEAT_INTERVAL_S = 1.0

# Пауза (в секундах), которая даёт устройствам время встать на шину:
PAUSE_S = 3.0

_speeds = {
    125: VSCAN_SPEED_125K,
    250: VSCAN_SPEED_250K,
    500: VSCAN_SPEED_500K,
    1000: VSCAN_SPEED_1M,
}
SPEED_ENUM = MappingProxyType(_speeds)
EMU = 0  # 0 - vs_can_api.dll, 2 - питон-сокеты
random.seed(2026)
# ============================================================
#                  ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
# ============================================================
def read_messages(bus, count, timeout):
    """Читает из bus до count сообщений, но не дольше timeout секунд."""
    start_t = time.perf_counter()
    msgs = []
    while len(msgs) < count and time.perf_counter() - start_t < timeout:
        chunk = bus.read_mes_s(count - len(msgs))
        if chunk:
            msgs.extend(chunk)
    return msgs

def dump_msg(msg, prefix=""):
    """Вывод на экран одного CAN-кадра (id, длина, данные)."""
    if msg is None:
        return f"{prefix}<нет сообщения>"
    msg_id = getattr(msg, "id", None)
    data = getattr(msg, "data", None)
    if msg_id is None or data is None:
        return f"{prefix}{msg!r}"
    data_hex = " ".join(f"{b:02X}" for b in data)
    return f"{prefix}id=0x{msg_id:08X} dlc={len(data)} data=[{data_hex}]"


def reset_channel(addr, speed=1000):
    """
    Открывает канал и сразу закрывает его
      addr  -- адрес устройства, например NETCAN_ADDR_1
      speed -- битрейт, Кбит/с, с которым канал будет на секунду открыт
    """
    with VSCAN(addr, emulation=EMU) as bus:
        bus.set_speed(SPEED_ENUM[speed])
        print(f"Канал {bus.port_com} открыт...")
    print(f"Канал {addr} закрыт")

def reset_all_channels(speed=1000):
    """Сбрасывает оба канала (NETCAN_ADDR_1 и NETCAN_ADDR_2) одним вызовом."""
    reset_channel(NETCAN_ADDR_1, speed)
    reset_channel(NETCAN_ADDR_2, speed)

# ============================================================
#                   ОСНОВНАЯ ОБЩАЯ ФУНКЦИЯ
# ============================================================
def send_and_check(bus_send, bus_recv, count=None, timeout=5, verbose=False):
    """
      bus_send -- откуда отправляем
      bus_recv -- куда должно прийти
      count    -- сколько кадров отправить
      timeout  -- сколько секунд ждать приёма
      verbose  -- если True, печатает каждый отправленный и принятый кадр

    Возвращает True, если все кадры дошли и совпали побайтово, иначе False.
    """
    if count is None:
        count = MSG_COUNT   #по умолчанию - задано в начале файла

    send_msgs = [VSCAN.form_message(TEST_CAN_ID, random.randbytes(MSG_DATA_SIZE)) for _ in range(count)]
    print(f"{bus_send.port_com} -> {bus_recv.port_com}  ({count} пакет(ов))")

    if verbose:     #если печатаем каждый кадр
        for i, msg in enumerate(send_msgs):
            print(dump_msg(msg, prefix=f"  send #{i}: "))

    #отправляем все кадры через устройство bus_send
    bus_send.write_mes_s(send_msgs)

    # слушаем bus_recv и собираем пришедшие кадры (не дольше timeout секунд)
    recv_msgs = read_messages(bus_recv, count, timeout)

    if verbose: #если печатаем каждый кадр
        for i, msg in enumerate(recv_msgs):
            print(dump_msg(msg, prefix=f"  recv #{i}: "))

    # сравниваем результат
    ok = bool(recv_msgs) and len(recv_msgs) == count and recv_msgs == send_msgs
    if ok:
        print(f"Пакеты успешно переданы: {count} из {count}")
    elif recv_msgs:
        print(f"<Error> Принято {len(recv_msgs)} из {count}")
    else:
        print(f"<Error> Ничего не принято за {timeout} с")
    print()
    return ok

# ============================================================
#                   ТЕСТ С НЕТКАНАМИ (и loopback)
# ============================================================
def test_open_only(speed=1000):
    """Просто открыть оба NetCAN и закрыть. Ничего не отправляет."""
    with VSCAN(NETCAN_ADDR_1, emulation=EMU) as bus1, VSCAN(NETCAN_ADDR_2, emulation=EMU) as bus2:
        bus1.set_speed(SPEED_ENUM[speed])
        bus2.set_speed(SPEED_ENUM[speed])
        print(f"Открыты каналы: {bus1.port_com}, {bus2.port_com}  ({speed} Kbit/s)")


def test_one_direction(speed=1000, reverse=False, loopback=False, count=None,
                       repeat=False, interval_s=None,
                       verbose=False, timeout=1,
                       addr1=None, addr2=None):
    """
    Отправка кадров в одну сторону. Два режима:

    loopback=False (по умолчанию) -- между двумя устройствами (addr1 <-> addr2).
    loopback=True -- одно устройство (addr1) в режиме self-reception

      speed      -- битрейт, Кбит/с (125/250/500/1000)
      reverse    -- False: addr1 -> addr2
                    True:  addr2 -> addr1
      loopback   -- True: проверка одного устройства самого на себя
      count      -- сколько кадров отправлять за один проход
      repeat     -- False: один проход и выход
                    True:  повторять раз в interval_s секунд, до Ctrl+C
      interval_s -- пауза между проходами при repeat=True;
      verbose    -- печатать каждый отправленный/принятый кадр
      timeout    -- сколько секунд ждать приёма
      addr1/addr2 -- адреса устройств; по умолчанию NETCAN_ADDR_1 / NETCAN_ADDR_2
                    (для loopback канала FDCAN2 передай addr1="192.168.0.17:2002")
    """
    if interval_s is None:
        interval_s = REPEAT_INTERVAL_S
    addr1 = addr1 or NETCAN_ADDR_1
    addr2 = addr2 or NETCAN_ADDR_2

    # ExitStack закрывает всё, что мы в него открыли, при выходе из блока --
    # так одним кодом обслуживаются и один канал (loopback), и два
    with ExitStack() as stack:
        if loopback:
            bus1 = stack.enter_context(VSCAN(addr1, mode=VSCAN_MODE_SELF_RECEPTION, emulation=EMU))
            bus1.set_speed(SPEED_ENUM[speed])
            bus_send = bus_recv = bus1       # шлёт сам себе и сам же принимает
        else:
            bus1 = stack.enter_context(VSCAN(addr1, emulation=EMU))
            bus2 = stack.enter_context(VSCAN(addr2, emulation=EMU))
            bus1.set_speed(SPEED_ENUM[speed])
            bus2.set_speed(SPEED_ENUM[speed])
            time.sleep(PAUSE_S)              # дать обоим устройствам встать на шину
            if reverse:
                bus_send, bus_recv = bus2, bus1  # reverse=True: bus2 шлёт, bus1 принимает
            else:
                bus_send, bus_recv = bus1, bus2  # reverse=False: bus1 шлёт, bus2 принимает

        if not repeat: #если не в цикле
            send_and_check(bus_send, bus_recv, count, timeout, verbose)
            return

        print(f"Периодическая отправка каждые {interval_s} с")
        try:
            while True:
                print(f"--- {time.strftime('%H:%M:%S')} ---")
                send_and_check(bus_send, bus_recv, count, timeout, verbose)
                time.sleep(interval_s)
        except KeyboardInterrupt:
            print("\nОстановлено (Ctrl+C)")

# ============================================================
#                   ТЕСТ ВСЕХ СКОРОСТЕЙ
# ============================================================
def test_all_speeds(addr1=None, addr2=None, both_directions=True,
                    count=None, timeout=5, speeds=None, verbose=False):
    """
    Проходит по всем скоростям и для каждой проверяет передачу между двумя
    устройствами. В конце печатает итоговую таблицу: какие скорости прошли.

      both_directions -- True: проверять в обе стороны (1->2 и 2->1)
                         False: только addr1 -> addr2
      count           -- сколько кадров отправлять на каждую скорость
                         (по умолчанию MSG_COUNT)
      timeout         -- сколько секунд ждать приёма
      speeds          -- список скоростей в Кбит/с, например [125, 500];
                         по умолчанию все из SPEED_ENUM
      verbose         -- печатать каждый отправленный/принятый кадр
    """
    addr1 = addr1 or NETCAN_ADDR_1
    addr2 = addr2 or NETCAN_ADDR_2
    speeds = speeds or sorted(SPEED_ENUM)

    print(f"Проверяем скорости (Кбит/с): {speeds}")
    results = []   # (скорость, направление, прошло ли)

    for speed in speeds:
        print(f"########## {speed} Kbit/s ##########")

        # Каналы открываются заново для каждой скорости
        with VSCAN(addr1, emulation=EMU) as bus1, VSCAN(addr2, emulation=EMU) as bus2:
            bus1.set_speed(SPEED_ENUM[speed])
            bus2.set_speed(SPEED_ENUM[speed])
            time.sleep(PAUSE_S)   # дать обоим устройствам встать на шину

            directions = [(bus1, bus2, "1 -> 2")]
            if both_directions:
                directions.append((bus2, bus1, "2 -> 1"))

            for bus_send, bus_recv, name in directions:
                ok = send_and_check(bus_send, bus_recv, count, timeout, verbose)
                results.append((speed, name, ok))

        time.sleep(PAUSE_S)   # дать плате время перед следующей скоростью

    # ---------- итоговая таблица ----------
    print("=" * 40)
    print("ИТОГ")
    print("=" * 40)
    for speed, name, ok in results:
        print(f"{speed:>5} Kbit/s  {name}:  {'OK' if ok else 'ОШИБКА'}")
    failed = [r for r in results if not r[2]]
    if failed:
        print(f"\nНе прошло: {len(failed)} из {len(results)}")
    else:
        print(f"\nВсе {len(results)} проверок прошли")
    print()

# ============================================================
#                   ТЕСТ С CANABLE
# ============================================================
def test_canable(speed_kbit=1000, reverse=False, count=MSG_COUNT, timeout=5, addr=None, port=None,
                 repeat=False, interval_s=None):
    """
    Обмен между платой (или NetCAN) и адаптером CANable.
      speed_kbit -- битрейт, Кбит/с
      reverse    -- False: плата шлёт -> ждём на CANable
                    True:  CANable шлёт -> ждём на плате
      count      -- сколько кадров отправить за один проход
      timeout    -- сколько секунд ждать приёма
      addr       -- адрес платы; по умолчанию host и порт берутся из NETCAN_ADDR_1
      port       -- TCP-порт платы (канал), если нужен другой, а не из NETCAN_ADDR_1;
                    addr при этом всё равно в приоритете, если задан
      repeat     -- False: один проход и выход
                    True:  повторять раз в interval_s секунд
      interval_s -- пауза между проходами при repeat=True;
                    по умолчанию REPEAT_INTERVAL_S сверху файла
    """
    if interval_s is None:
        interval_s = REPEAT_INTERVAL_S

    if port is None:
        port = NETCAN_ADDR_1.split(':')[1]  # порт берётся из адреса в начале файла
    addr = addr or f"{NETCAN_ADDR_1.split(':')[0]}:{port}"

    # открываем соединение с платой
    with VSCAN(addr, emulation=EMU) as board:
        board.set_speed(SPEED_ENUM[speed_kbit])  # ставим скорость

        # открываем соединение с CANable (по COM-порту, через python-can)
        canable = can.interface.Bus(interface='slcan', channel=CANABLE_PORT, bitrate=speed_kbit * 1000)

        # один проход, без цикла
        def one_pass():
            if reverse:  # ветка "CANable шлёт -> плата принимает"
                print(f"=== CANable ({CANABLE_PORT}) -> Плата ({addr}), {speed_kbit} Kbit/s ===")

                # CANable шлёт count кадров, по одному
                for i in range(count):
                    msg = can.Message(arbitration_id=TEST_CAN_ID,
                                      data=random.randbytes(MSG_DATA_SIZE),
                                      is_extended_id=True)
                    canable.send(msg)
                    print(f"  send #{i}: {msg}")

                # плата слушает и собирает пришедшие кадры
                received = read_messages(board, count, timeout)
                for i, m in enumerate(received):
                    print(f"  recv #{i}: {m}")

            else:  # ветка "плата шлёт -> CANable принимает"
                print(f"=== Плата ({addr}) -> CANable ({CANABLE_PORT}), {speed_kbit} Kbit/s ===")

                # плата шлёт все count кадров разом
                send_msgs = [VSCAN.form_message(TEST_CAN_ID, random.randbytes(MSG_DATA_SIZE)) for _ in range(count)]
                for i, m in enumerate(send_msgs):
                    print(f"  send #{i}: {m}")
                board.write_mes_s(send_msgs)

                # CANable слушает и собирает пришедшие кадры, пока не наберёт count или не выйдет timeout
                received = []
                t0 = time.perf_counter()
                while len(received) < count and time.perf_counter() - t0 < timeout:
                    msg = canable.recv(timeout=0.2)
                    if msg is not None:
                        received.append(msg)
                        print(f"  recv #{len(received) - 1}: {msg}")

            print(f"\nИтог: отправлено {count}, принято {len(received)}")
            if received:
                print(f"Пакеты успешно переданы: {len(received)} из {count}")
            else:
                print("Ничего не принято")
            print()

        try:
            if not repeat:  # обычный режим -- один проход и выход
                one_pass()
                return

            # режим цикла -- повторяем one_pass() раз в interval_s секунд
            print(f"Периодическая отправка каждые {interval_s} с")
            try:
                while True:
                    print(f"--- {time.strftime('%H:%M:%S')} ---")
                    one_pass()
                    time.sleep(interval_s)
            except KeyboardInterrupt:
                print("\nОстановлено (Ctrl+C)")
        finally:
            canable.shutdown()


# ============================================================
#                   НАГРУЗОЧНЫЙ ТЕСТ
# ============================================================
def _load_pass(bus_send, bus_recv, speed, count, timeout):
    """
    Один прогон нагрузочного теста: шлёт count кадров разом через bus_send,
    ждёт их на bus_recv (не дольше timeout с) и меряет время передачи.

    Возвращает (pps, kbps, received, ok):
      pps      -- пакетов в секунду
      kbps     -- килобайт в секунду
      received -- сколько кадров реально дошло
      ok       -- True, если дошли все count кадров и совпали побайтово
    """
    send_msgs = [VSCAN.form_message(TEST_CAN_ID, random.randbytes(MSG_DATA_SIZE)) for _ in range(count)]
    print(f"{bus_send.port_com} -> {bus_recv.port_com}  {count} кадров, {speed} Kbit/s")

    # засекаем время только вокруг самой передачи -- формирование
    # кадров выше в расчёт не идёт, оно не часть скорости канала
    t0 = time.perf_counter()
    bus_send.write_mes_s(send_msgs)
    recv_msgs = read_messages(bus_recv, count, timeout)
    t1 = time.perf_counter()

    dt = t1 - t0
    received = len(recv_msgs)
    pps = received / dt if dt > 0 else 0.0
    kbps = received * MSG_DATA_SIZE / 1024 / dt if dt > 0 else 0.0
    ok = received == count and recv_msgs == send_msgs

    print(f"Время: {dt:.4f} с")
    print(f"Скорость: {pps:.1f} пакет/с, {kbps:.2f} КБ/с")
    print(f"Доставлено: {received} из {count}")
    if ok:
        print("Все кадры дошли и совпадают побайтово -- OK")
    elif received:
        print("<Error> Часть кадров потеряна или данные не совпадают")
    else:
        print("<Error> Ничего не принято")
    print()

    return pps, kbps, received, ok


def test_load(addr1=None, addr2=None, speeds=None, reverse=False, count=100, timeout=30):
    """
    Нагрузочный тест: поочерёдно проходит по всем (или заданным) скоростям,
    на каждой отправляет count кадров подряд и меряет скорость передачи
    (пакетов/с, КБ/с). В конце печатает сводную таблицу по всем скоростям.

      addr1/addr2 -- адреса двух устройств
      speeds      -- список скоростей в Кбит/с, например [125, 500];
                     по умолчанию все из SPEED_ENUM
      reverse     -- False: addr1 -> addr2, True: addr2 -> addr1
      count       -- сколько кадров отправить подряд на каждой скорости
                     (для нагрузочного теста имеет смысл взять побольше,
                     например 100-1000)
      timeout     -- сколько секунд ждать приёма всех кадров
    """
    addr1 = addr1 or NETCAN_ADDR_1
    addr2 = addr2 or NETCAN_ADDR_2
    speeds = speeds or sorted(SPEED_ENUM)

    print(f"Нагрузочный тест, скорости (Кбит/с): {speeds}")
    results = []   # (скорость, пакет/с, КБ/с, доставлено, прошло ли)

    for speed in speeds:
        print(f"########## {speed} Kbit/s ##########")

        # каналы открываются заново для каждой скорости
        with VSCAN(addr1, emulation=EMU) as bus1, VSCAN(addr2, emulation=EMU) as bus2:
            bus1.set_speed(SPEED_ENUM[speed])
            bus2.set_speed(SPEED_ENUM[speed])
            bus_send, bus_recv = (bus2, bus1) if reverse else (bus1, bus2)
            time.sleep(PAUSE_S)   # дать обоим устройствам встать на шину

            pps, kbps, received, ok = _load_pass(bus_send, bus_recv, speed, count, timeout)
            results.append((speed, pps, kbps, received, count, ok))

        time.sleep(PAUSE_S)   # дать плате время перед следующей скоростью

    # ---------- итоговая таблица ----------
    print("=" * 60)
    print("ИТОГ (нагрузочный тест)")
    print("=" * 60)
    for speed, pps, kbps, received, count, ok in results:
        status = "OK" if ok else "ОШИБКА"
        print(f"{speed:>5} Kbit/s   {pps:>8.1f} пакет/с   {kbps:>8.2f} КБ/с   "
              f"{received}/{count}   {status}")
    failed = [r for r in results if not r[5]]
    if failed:
        print(f"\nНе прошло: {len(failed)} из {len(results)}")
    else:
        print(f"\nВсе {len(results)} проверок прошли")
    print()


# ============================================================
#          ПОИСК МАКСИМАЛЬНОЙ ПРОПУСКНОЙ СПОСОБНОСТИ
# ============================================================
def test_find_max_throughput(addr1=None, addr2=None, speed=1000, reverse=False,
                             counts=None, timeout=10):
    """
    Находит практический предел: на заданной скорости шины (speed) постепенно
    увеличивает количество кадров в одной пачке (count) и смотрит, на каком
    объёме начинаются потери. Останавливается на первой неудаче -- дальше
    только хуже, повторять нет смысла.

      addr1/addr2 -- адреса двух устройств
      speed       -- битрейт шины, Кбит/с, на котором ищем предел
      reverse     -- False: addr1 -> addr2, True: addr2 -> addr1
      counts      -- список размеров пачки кадров, по возрастанию;
                     по умолчанию [10, 50, 100, 250, 500, 1000, 2000, 5000]
      timeout     -- сколько секунд ждать приёма каждой пачки

    В конце печатает таблицу всех прогонов и вывод: последний count,
    на котором всё прошло без потерь -- это и есть практический максимум
    для данного тракта (плата + TCP/SLCAN + ПК), а не предел самой шины.
    """
    addr1 = addr1 or NETCAN_ADDR_1
    addr2 = addr2 or NETCAN_ADDR_2
    counts = counts or [10, 50, 100, 250, 500, 1000, 2000, 5000]

    print(f"Поиск максимальной пропускной способности на {speed} Kbit/s")
    print(f"Пробуем размеры пачки: {counts}")
    results = []   # (count, пакет/с, КБ/с, доставлено, прошло ли)
    last_good = None

    # канал открывается один раз и держится открытым на всю серию --
    # так измеряется именно рост нагрузки, без лишних переоткрытий
    with VSCAN(addr1, emulation=EMU) as bus1, VSCAN(addr2, emulation=EMU) as bus2:
        bus1.set_speed(SPEED_ENUM[speed])
        bus2.set_speed(SPEED_ENUM[speed])
        bus_send, bus_recv = (bus2, bus1) if reverse else (bus1, bus2)
        time.sleep(PAUSE_S)   # дать обоим устройствам встать на шину

        for count in counts:
            print(f"########## count = {count} ##########")
            pps, kbps, received, ok = _load_pass(bus_send, bus_recv, speed, count, timeout)
            results.append((count, pps, kbps, received, ok))

            if ok:
                last_good = count
            else:
                print(f"<Стоп> Потери начались на count={count}, дальше не идём")
                break

            time.sleep(PAUSE_S)   # небольшая пауза между пачками

    # ---------- итоговая таблица ----------
    print("=" * 60)
    print(f"ИТОГ (поиск максимума, {speed} Kbit/s)")
    print("=" * 60)
    for count, pps, kbps, received, ok in results:
        status = "OK" if ok else "ОШИБКА"
        print(f"count={count:>5}   {pps:>8.1f} пакет/с   {kbps:>8.2f} КБ/с   "
              f"{received}/{count}   {status}")

    if last_good is None:
        print("\nДаже наименьшая пачка не прошла без потерь -- проблема раньше, чем в объёме")
    elif last_good == counts[-1]:
        print(f"\nВсе пачки до count={last_good} прошли без потерь -- "
              f"предел ещё не найден, попробуйте увеличить counts")
    else:
        best_pps = next(pps for c, pps, kbps, received, ok in results if c == last_good)
        print(f"\nПрактический максимум без потерь: count={last_good} "
              f"(~{best_pps:.0f} пакет/с на {speed} Kbit/s)")
    print()

# ============================================================
#                           MAIN
# ============================================================
if __name__ == "__main__":
    # Открыть канал (без отправки данных):
    #test_open_only(speed=1000)

    '''
    for i in range(10):
        print(f"===== Прогон {i + 1}/10 =====")
        test_one_direction(speed=1000, reverse=True, repeat=False, verbose=True)
        test_one_direction(speed=1000, reverse=False, repeat=False, verbose=True)
    '''

    # test_one_direction(speed=1000, loopback=False, repeat=True, verbose=True)

    # Сбросить оба канала одним вызовом (закрыть-переоткрыть каждый):
    # reset_all_channels()

    # Плата <-> CANable: False: плата шлёт -> ждём на CANable
    #                    True:  CANable шлёт -> ждём на плате
    # test_canable(speed_kbit=1000, reverse=True, count=MSG_COUNT, repeat=False)
    # test_canable(speed_kbit=1000, reverse=False, count=MSG_COUNT, repeat=False)

    # Все скорости между NETCAN_ADDR_1 и NETCAN_ADDR_2
    # test_all_speeds()
    # test_all_speeds(both_directions=False)

    # Нагрузочный тест по всем скоростям (сводная таблица в конце):
    # отправка всех count кадров разом
    test_load(reverse=False, count=100)
    # test_load(speeds=[125, 500], reverse=False, count=100)

    # Поиск максимальной пропускной способности
    # test_find_max_throughput(speed=1000, reverse=False)
    # test_find_max_throughput(speed=1000, reverse=False, counts=[10000, 15000, 18500, 20000])

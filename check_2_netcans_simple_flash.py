"""Тест производительности передачи данных между двумя устройствами NetCAN."""
import random
import time
import can  # python-can
import vs_can_lib
import threading
import struct
from contextlib import ExitStack
from types import MappingProxyType
from vs_can_lib import VSCAN, VSCAN_SPEED_1M, VSCAN_SPEED_125K, VSCAN_SPEED_250K, VSCAN_SPEED_500K, \
    VSCAN_MODE_SELF_RECEPTION
# ============================================================
#                        НАСТРОЙКИ
# ============================================================
NETCAN_ADDR_1 = "192.168.0.18:2001"
NETCAN_ADDR_2 = "192.168.0.18:2002"
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
def _load_pass(bus_send, bus_recv, speed, count, timeout, duration=None):
    """
    Один прогон нагрузочного теста. Два режима:

    duration=None (по умолчанию, как раньше) -- одноразовый "залп": шлёт
    count кадров разом через bus_send, ждёт их на bus_recv (не дольше
    timeout с) и меряет время передачи этой ОДНОЙ пачки. Это измеряет,
    сколько влезает в один burst -- не то же самое, что устойчивая
    (sustained) скорость в установившемся режиме.

    duration=<секунды> -- вместо одного залпа гонит пачки по count кадров
    подряд, одну за другой, без пауз между ними, пока не наберётся
    duration секунд суммарного времени, и считает pps/kbps по ВСЕМ пачкам
    сразу. Это и есть устойчивая скорость -- к ней в итоге и сходится
    система при долгой непрерывной нагрузке, в отличие от предела одного
    мгновенного burst'а.

      bus_send -- откуда отправляем
      bus_recv -- куда должно прийти
      speed    -- битрейт, Кбит/с (только для печати)
      count    -- размер одной пачки кадров
      timeout  -- сколько секунд ждать приёма ОДНОЙ пачки
      duration -- если задано (сек) -- суммарная длительность всего
                  прогона; если None -- одна пачка и выход (старое
                  поведение, полностью совместимо с прежними вызовами)

    Возвращает (pps, kbps, received, ok) -- как и раньше. При duration
    received -- сумма по всем пачкам, pps/kbps посчитаны по суммарному
    времени всего прогона, а не одной пачки; ok становится False сразу,
    как только хоть одна пачка внутри прогона потеряется или не совпадёт
    (дальше прогон останавливается -- это и есть найденный предел
    устойчивой нагрузки).
    """
    if duration is None:
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
    else:
        print(f"{bus_send.port_com} -> {bus_recv.port_com}  устойчивая нагрузка "
              f"{duration:.1f} с, пачками по {count} кадров, {speed} Kbit/s")

        total_received = 0
        ok = True
        t_start = time.perf_counter()

        while (time.perf_counter() - t_start) < duration:
            send_msgs = [VSCAN.form_message(TEST_CAN_ID, random.randbytes(MSG_DATA_SIZE)) for _ in range(count)]
            bus_send.write_mes_s(send_msgs)
            recv_msgs = read_messages(bus_recv, count, timeout)
            total_received += len(recv_msgs)

            if len(recv_msgs) != count or recv_msgs != send_msgs:
                ok = False
                print(f"<Error> Пачка потеряна/не совпала (принято {len(recv_msgs)} из {count}) "
                      f"-- прерываю устойчивый прогон")
                break

        dt = time.perf_counter() - t_start
        received = total_received
        pps = received / dt if dt > 0 else 0.0
        kbps = received * MSG_DATA_SIZE / 1024 / dt if dt > 0 else 0.0

    print(f"Время: {dt:.4f} с")
    print(f"Скорость: {pps:.1f} пакет/с, {kbps:.2f} КБ/с")
    print(f"Доставлено: {received}" + (f" из {count}" if duration is None else " (суммарно)"))
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
                             loopback=False, counts=None, timeout=10, sustain_s=None):
    """
    Находит практический предел: на заданной скорости шины (speed) постепенно
    увеличивает количество кадров в одной пачке (count) и смотрит, на каком
    объёме начинаются потери. Останавливается на первой неудаче -- дальше
    только хуже, повторять нет смысла.

    Два режима подключения (как в test_one_direction):
    loopback=False (по умолчанию) -- между двумя устройствами (addr1 <-> addr2).
    loopback=True -- одно устройство (addr1) в режиме self-reception, шлёт
                     само себе и сама же принимает (addr2 в этом режиме не
                     используется).

      addr1/addr2 -- адреса устройств; при loopback=True используется
                     только addr1 (для канала FDCAN2 передай
                     addr1="192.168.0.17:2002")
      speed       -- битрейт шины, Кбит/с, на котором ищем предел
      reverse     -- используется только при loopback=False:
                     False: addr1 -> addr2, True: addr2 -> addr1
      loopback    -- True: проверка одного устройства самого на себя
      counts      -- список размеров пачки кадров, по возрастанию;
                     по умолчанию [10, 50, 100, 250, 500, 1000, 2000, 5000]
      timeout     -- сколько секунд ждать приёма каждой пачки
      sustain_s   -- если задано (сек): после того, как найден last_good
                     (максимальный размер пачки, прошедший без потерь),
                     дополнительно прогоняет именно его в РЕЖИМЕ УСТОЙЧИВОЙ
                     нагрузки (пачки по last_good кадров подряд, без пауз,
                     в течение sustain_s секунд) -- это проверяет, держит ли
                     плата найденный предел не только на одном коротком
                     burst'е, а и в установившемся непрерывном потоке.
                     Если None (по умолчанию) -- эта проверка не делается,
                     поведение полностью как раньше.

    В конце печатает таблицу всех прогонов и вывод: последний count,
    на котором всё прошло без потерь -- это и есть практический максимум
    для данного тракта (плата + TCP/SLCAN + ПК), а не предел самой шины.
    Если задан sustain_s -- следом печатает результат устойчивой проверки.
    """
    addr1 = addr1 or NETCAN_ADDR_1
    addr2 = addr2 or NETCAN_ADDR_2
    counts = counts or [10, 50, 100, 250, 500, 1000, 2000, 5000]

    print(f"Поиск максимальной пропускной способности на {speed} Kbit/s"
          + (" (loopback)" if loopback else ""))
    print(f"Пробуем размеры пачки: {counts}")
    results = []   # (count, пакет/с, КБ/с, доставлено, прошло ли)
    last_good = None

    # канал(ы) открываются один раз и держатся открытыми на всю серию --
    # так измеряется именно рост нагрузки, без лишних переоткрытий
    with ExitStack() as stack:
        if loopback:
            bus1 = stack.enter_context(VSCAN(addr1, mode=VSCAN_MODE_SELF_RECEPTION, emulation=EMU))
            bus1.set_speed(SPEED_ENUM[speed])
            bus_send = bus_recv = bus1
        else:
            bus1 = stack.enter_context(VSCAN(addr1, emulation=EMU))
            bus2 = stack.enter_context(VSCAN(addr2, emulation=EMU))
            bus1.set_speed(SPEED_ENUM[speed])
            bus2.set_speed(SPEED_ENUM[speed])
            bus_send, bus_recv = (bus2, bus1) if reverse else (bus1, bus2)
        time.sleep(PAUSE_S)   # дать устройству(ам) встать на шину

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

    # ---------- проверка на устойчивую (sustained) нагрузку ----------
    if sustain_s is not None and last_good is not None:
        print("=" * 60)
        print(f"ПРОВЕРКА УСТОЙЧИВОЙ НАГРУЗКИ (count={last_good}, {sustain_s:.0f} с)")
        print("=" * 60)

        # канал(ы) открываются заново -- отдельный, чистый прогон
        with ExitStack() as stack:
            if loopback:
                bus1 = stack.enter_context(VSCAN(addr1, mode=VSCAN_MODE_SELF_RECEPTION, emulation=EMU))
                bus1.set_speed(SPEED_ENUM[speed])
                bus_send = bus_recv = bus1
            else:
                bus1 = stack.enter_context(VSCAN(addr1, emulation=EMU))
                bus2 = stack.enter_context(VSCAN(addr2, emulation=EMU))
                bus1.set_speed(SPEED_ENUM[speed])
                bus2.set_speed(SPEED_ENUM[speed])
                bus_send, bus_recv = (bus2, bus1) if reverse else (bus1, bus2)
            time.sleep(PAUSE_S)   # дать устройству(ам) встать на шину

            sus_pps, sus_kbps, sus_received, sus_ok = _load_pass(
                bus_send, bus_recv, speed, last_good, timeout, duration=sustain_s)

        if sus_ok:
            print(f"\nУстойчивая скорость подтверждена: ~{sus_pps:.0f} пакет/с "
                  f"({sus_kbps:.2f} КБ/с) непрерывно в течение {sustain_s:.0f} с "
                  f"на count={last_good}, на {speed} Kbit/s")
        else:
            print(f"\n<Внимание> На устойчивой нагрузке (count={last_good}) потери "
                  f"всё же появились -- практический устойчивый максимум ниже, чем "
                  f"{last_good}, найденный короткими пачками. Полученная за это время "
                  f"скорость: ~{sus_pps:.0f} пакет/с ({sus_kbps:.2f} КБ/с)")
        print()

# ============================================================
#                 ТЕСТ НЕПРЕРЫВНЫЙ ПОТОК ДАННЫХ
# ============================================================
# ============================================================
#                 ТЕСТ НЕПРЕРЫВНЫЙ ПОТОК ДАННЫХ
# ============================================================
def test_continuous_batches(addr1=None, addr2=None, speed=1000, reverse=False,
                            duration=60, batch_size=300, gap_warn_ms=5.0, drain_idle_s=2.0):
    """
    Непрерывная отправка пачками без ожидания эха: sender и receiver
    работают в отдельных потоках.

    Каждый кадр несёт в Data: байты 0-3 -- порядковый номер seq (big-endian),
    байты 4-7 -- seq ^ 0xA5A5A5A5 (контроль целостности всех 8 байт без
    хранения отправленных кадров в памяти). Требует MSG_DATA_SIZE == 8.

      duration     -- сколько секунд непрерывно слать
      batch_size   -- размер одной пачки
      gap_warn_ms  -- порог (мс) простоя Python между пачками (от конца одного
                      write_mes_s до начала следующего), после которого
                      печатается предупреждение. Время, когда write_mes_s
                      заблокирован, паузой НЕ считается -- это back-pressure,
                      значит буферы полны и шина занята.
      drain_idle_s -- после остановки sender'а: сколько секунд без новых
                      кадров ждать, прежде чем считать приём законченным
    """
    addr1 = addr1 or NETCAN_ADDR_1
    addr2 = addr2 or NETCAN_ADDR_2
    check_xor = 0xA5A5A5A5

    def make_data(seq):
        return struct.pack(">II", seq & 0xFFFFFFFF, (seq ^ check_xor) & 0xFFFFFFFF)

    with VSCAN(addr1, emulation=EMU) as bus1, VSCAN(addr2, emulation=EMU) as bus2:
        bus1.set_speed(SPEED_ENUM[speed])
        bus2.set_speed(SPEED_ENUM[speed])
        bus_send, bus_recv = (bus2, bus1) if reverse else (bus1, bus2)
        print(f"{bus_send.port_com} -> {bus_recv.port_com}  непрерывный поток "
              f"{duration} с, пачками по {batch_size}, {speed} Kbit/s")
        time.sleep(PAUSE_S)

        st = {
            "sent": 0, "received": 0, "corrupted": 0, "seq_breaks": 0, "lost": 0,
            "next_expected": 0, "gaps_detected": 0, "max_gap_ms": 0.0,
            "t_send_start": None, "t_send_end": None, "t_first_rx": None, "t_last_rx": None,
            "sender_error": None, "receiver_error": None,
        }
        sender_done = threading.Event()   # sender закончил (штатно или аварийно)
        abort = threading.Event()         # receiver упал -- sender должен остановиться

        def sender():
            seq = 0
            prev_end = None
            st["t_send_start"] = time.perf_counter()
            try:
                while not abort.is_set() and (time.perf_counter() - st["t_send_start"]) < duration:
                    batch = [VSCAN.form_message(TEST_CAN_ID, make_data(seq + i)) for i in range(batch_size)]

                    t_before = time.perf_counter()
                    if prev_end is not None:
                        gap_ms = (t_before - prev_end) * 1000.0
                        st["max_gap_ms"] = max(st["max_gap_ms"], gap_ms)
                        if gap_ms > gap_warn_ms:
                            st["gaps_detected"] += 1
                            # print(f"<Пауза> простой Python между пачками {gap_ms:.2f} мс "
                            #      f"(порог {gap_warn_ms:.1f} мс), seq={seq}")

                    bus_send.write_mes_s(batch)
                    prev_end = time.perf_counter()
                    seq += batch_size
                    st["sent"] = seq
            except Exception as e:
                st["sender_error"] = e
                print(f"<Error> sender упал: {e!r} (успешно отправлено: {st['sent']})")
            finally:
                st["t_send_end"] = time.perf_counter()
                sender_done.set()

        def receiver():
            last_rx = time.perf_counter()
            try:
                while not abort.is_set():
                    chunk = bus_recv.read_mes_s(batch_size)
                    now = time.perf_counter()
                    if chunk:
                        last_rx = now
                        if st["t_first_rx"] is None:
                            st["t_first_rx"] = now
                        st["t_last_rx"] = now
                        for msg in chunk:
                            seq, check = struct.unpack(">II", bytes(msg.Data))
                            if (msg.Id != TEST_CAN_ID or msg.Size != MSG_DATA_SIZE
                                    or check != (seq ^ check_xor) & 0xFFFFFFFF):
                                st["corrupted"] += 1
                                if st["corrupted"] <= 10:
                                    print(f"<Error> битый кадр: {msg!r}")
                                continue
                            if seq != st["next_expected"]:
                                st["seq_breaks"] += 1
                                if seq > st["next_expected"]:
                                    st["lost"] += seq - st["next_expected"]
                                print(f"<Error> разрыв: ожидали {st['next_expected']}, пришло {seq}")
                            st["next_expected"] = seq + 1
                            st["received"] += 1
                    else:
                        if sender_done.is_set() and (st["received"] >= st["sent"]
                                                     or now - last_rx > drain_idle_s):
                            break
                        time.sleep(0)   # отдать GIL sender'у, не засыпая на 15 мс
            except Exception as e:
                st["receiver_error"] = e
                print(f"<Error> receiver упал: {e!r} (принято к этому моменту: {st['received']})")
                abort.set()

        t_recv = threading.Thread(target=receiver)
        t_send = threading.Thread(target=sender)
        t_recv.start()
        t_send.start()
        t_send.join()
        t_recv.join()

    # ---------- итог ----------
    send_dt = st["t_send_end"] - st["t_send_start"]
    rx_dt = (st["t_last_rx"] - st["t_first_rx"]) if st["t_first_rx"] is not None else 0.0
    send_pps = st["sent"] / send_dt if send_dt > 0 else 0.0
    rx_pps = st["received"] / rx_dt if rx_dt > 0 else 0.0
    failed = st["sender_error"] is not None or st["receiver_error"] is not None

    print()
    print(f"Отправлено: {st['sent']} за {send_dt:.2f} с  ({send_pps:.1f} пакет/с)")
    print(f"Принято:    {st['received']} за {rx_dt:.2f} с  ({rx_pps:.1f} пакет/с, ")
    print(f"Битых кадров: {st['corrupted']}")
    print(f"Разрывов последовательности: {st['seq_breaks']} (пропущено кадров: {st['lost']})")
    print(f"Простой Python между пачками: макс. {st['max_gap_ms']:.2f} мс, "
          f"превышений порога {gap_warn_ms:.1f} мс: {st['gaps_detected']}")

    if failed:
        print("<Error> тест завершился аварийно")
        return

    if st["received"] == st["sent"] and st["seq_breaks"] == 0 and st["corrupted"] == 0:
        print("OK -- все кадры дошли, по порядку, без искажений")
    else:
        print(f"<Error> недостача {st['sent'] - st['received']} кадров")

    if st["gaps_detected"] == 0:
        print("Python-сторона: простоев выше порога нет")
    else:
        print("Python-сторона: были простои выше порога")

# ============================================================
#      ТЕСТ: ПРИ КАКОМ РАЗМЕРЕ ПАЧКИ ПЛАТА ЗАКРЫВАЕТ ОКНО
# ============================================================
def test_zero_window_series(addr1=None, addr2=None, speed=1000,
                            counts=None, repeats=3, pause_s=2.0, timeout=5):
    """
    Отправляет пачки разного размера с паузами между ними (ждёт приёма
    каждой пачки, затем пауза pause_s). Нужен для захвата в Wireshark:
    видно, при каком размере пачки плата объявляет окно 0.

    Ожидание: окно платы 5840 байт = ~216 кадров по 27 байт. Пачки меньше
    этого целиком влезают в окно -- нулевого окна быть не должно. Пачки
    больше -- ПК упирается в окно в каждой пачке.

    Перед каждой серией печатает время (perf_counter), чтобы в Wireshark
    было легко найти, где какая пачка (по паузам pause_s между ними).

      counts  -- размеры пачек по порядку
      repeats -- сколько раз повторить каждый размер
      pause_s -- пауза между пачками (секунды)
    """
    addr1 = addr1 or NETCAN_ADDR_1
    addr2 = addr2 or NETCAN_ADDR_2
    counts = counts or [50, 100, 150, 200, 210, 220, 250, 300, 500, 1000]

    with VSCAN(addr1, emulation=EMU) as bus1, VSCAN(addr2, emulation=EMU) as bus2:
        bus1.set_speed(SPEED_ENUM[speed])
        bus2.set_speed(SPEED_ENUM[speed])
        time.sleep(PAUSE_S)

        t0 = time.perf_counter()
        print(f"Серия пачек с паузой {pause_s} с, каждый размер x{repeats}")
        for count in counts:
            for r in range(repeats):
                t = time.perf_counter() - t0
                print(f"--- t={t:8.3f} с  count={count:5d} ({count * 27} байт)  повтор {r + 1}/{repeats}")
                send_and_check(bus1, bus2, count=count, timeout=timeout)
                time.sleep(pause_s)

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

    # test_one_direction(speed=1000, loopback=False, repeat=False, verbose=True)

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
    # test_load(reverse=False, count=100)
    # test_load(speeds=[125, 500], reverse=False, count=100)

    # Поиск максимальной пропускной способности
    # test_find_max_throughput(speed=1000, reverse=False)
    #test_find_max_throughput(speed=1000, reverse=False, counts=[300], loopback=False, sustain_s=4800)

    # test_one_direction(speed=1000, loopback=False, repeat=False, count=300, timeout=5)
    test_continuous_batches(speed=1000, reverse=False, duration=15000, batch_size=1000)
    # test_continuous_batches(speed=1000, reverse=False, duration=5400, batch_size=300)

    # test_zero_window_series(speed=1000)
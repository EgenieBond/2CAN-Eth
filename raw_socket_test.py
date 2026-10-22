"""
Нагрузочный тест NetCAN БЕЗ vs_can_lib / vs_can_api.dll -- на чистых Python-сокетах.

Зачем: тест через vs_can_lib упирается во внутреннюю буферизацию и (возможно)
глобальную блокировку vs_can_api.dll -- при одновременной записи и чтении из
двух потоков DLL может сериализовать вызовы, и тогда тест показывает потери,
которых на шине нет. Этот скрипт обходит библиотеку целиком: сам формирует
ASCII-команды SLCAN и сам шлёт/читает их через обычные сокеты. У отправителя и
получателя свои сокеты, общих блокировок нет, sendall()/recv() отпускают GIL.

Два теста:
  raw_find_max_throughput() -- burst: шлём пачку, ждём её приёма, растим размер
                               пачки, пока не начнутся потери.
  raw_continuous_test()     -- настоящий непрерывный поток: отправка, чтение
                               подтверждений и приём идут в трёх независимых
                               потоках, ничего не дожидаясь друг друга.

Протокол (SLCAN):
  C\\r                                       -- закрыть канал
  S<код>\\r                                  -- скорость (код 8 = 1 Мбит/с)
  O\\r                                       -- открыть канал, обычный режим
  T<8 hex ID><1 hex DLC><2*DLC hex data>\\r  -- отправка расширенного кадра
  Ответ платы отправителю: "Z\\r" -- кадр принят в очередь на CAN,
                           "\\a"  -- кадр отвергнут.
  Получателю приходят принятые с шины кадры в том же формате T...\\r.
"""
import random
import select
import socket
import struct
import threading
import time

# ============================================================
#                        НАСТРОЙКИ
# ============================================================
NETCAN_HOST = "192.168.0.18"
PORT_CH0 = 2001   # FDCAN1 -- отправка
PORT_CH1 = 2002   # FDCAN2 -- приём

MSG_DATA_SIZE = 8
TEST_CAN_ID = 0x10203040
CONNECT_TIMEOUT_S = 5.0
PAUSE_S = 3.0      # дать устройству встать на шину после O\r

BUS_CEILING_PPS = 4979   # эмпирический потолок шины 1 Мбит/с, ext ID, 8 байт данных

SPEED_CODE = {125: 4, 250: 5, 500: 6, 1000: 8}

CHECK_XOR = 0xA5A5A5A5
MASK32 = 0xFFFFFFFF

random.seed(2026)


# ============================================================
#                  ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
# ============================================================
def _drain_socket(sock):
    """Вычитывает и выбрасывает всё, что уже лежит в сокете (служебные ответы \\r, старые Z\\r и т.п.)."""
    old_timeout = sock.gettimeout()
    sock.settimeout(0.0)
    try:
        while True:
            try:
                chunk = sock.recv(65536)
            except (BlockingIOError, socket.timeout):
                break
            if not chunk:
                break
    finally:
        sock.settimeout(old_timeout)


def _open_channel(host, port, speed_kbit=1000, timeout=5.0):
    """Открывает TCP-соединение и переводит канал на шину (C + S<код> + O)."""
    sock = socket.create_connection((host, port), timeout=CONNECT_TIMEOUT_S)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(timeout)
    # C\r -- закрыть канал, если он остался открытым после аварийного прогона
    # (смена скорости на открытом канале вернула бы \a). Одиночный \r перед
    # ним не нужен: у нового соединения на плате буфер строки и так пустой,
    # а пустая строка только даёт "parse ERROR" в логе платы.
    sock.sendall(b"C\r")
    sock.sendall(f"S{SPEED_CODE[speed_kbit]}\r".encode("ascii"))
    sock.sendall(b"O\r")
    time.sleep(PAUSE_S)
    _drain_socket(sock)   # выбросить ответы \r на служебные команды
    return sock


def _close_channel(sock):
    try:
        sock.settimeout(1.0)
        sock.sendall(b"C\r")
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


def _make_frame_ascii(can_id, data):
    """Собирает один кадр в ASCII SLCAN-формате: T + 8 hex ID + 1 hex DLC + data + \\r."""
    return b"T%08X%X%s\r" % (can_id, len(data), data.hex().upper().encode("ascii"))


def _make_seq_data(seq):
    """8 байт данных кадра: байты 0-3 -- seq, байты 4-7 -- seq ^ 0xA5A5A5A5 (контроль целостности)."""
    return struct.pack(">II", seq & MASK32, (seq ^ CHECK_XOR) & MASK32)


def _parse_frames(buf):
    """
    Разбирает накопленный буфер байт на кадры вида T........\\r.
    Возвращает (список кадров как (id, data_bytes), недопринятый хвост).
    Один split вместо побайтового среза -- линейно по размеру буфера
    (побайтовый срез buf = buf[idx+1:] на каждой строке был квадратичным
    и при непрерывном потоке не успевал за шиной).
    """
    parts = buf.split(b"\r")
    tail = parts.pop()            # последний кусок -- строка без \r, ещё не дошла целиком
    frames = []
    for line in parts:
        line = line.lstrip(b"\x07")   # на случай BELL перед строкой
        if line.startswith(b"T") and len(line) >= 10:
            try:
                can_id = int(line[1:9], 16)
                dlc = int(line[9:10], 16)
                data = bytes.fromhex(line[10:10 + dlc * 2].decode("ascii"))
                frames.append((can_id, data))
            except ValueError:
                pass  # не наш формат строки -- пропускаем
    return frames, tail


def _read_frames(sock, count, timeout):
    """Читает из сокета, пока не наберёт count кадров или не выйдет timeout."""
    frames = []
    buf = b""
    old_timeout = sock.gettimeout()
    sock.settimeout(0.2)
    t0 = time.perf_counter()
    try:
        while len(frames) < count and (time.perf_counter() - t0) < timeout:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            new_frames, buf = _parse_frames(buf + chunk)
            frames.extend(new_frames)
    finally:
        sock.settimeout(old_timeout)
    return frames


# ============================================================
#                  BURST-ТЕСТ (поиск максимума пачки)
# ============================================================
def raw_load_pass(sock_send, sock_recv, speed, count, timeout):
    """
    Один прогон: собирает count кадров в ОДИН буфер байт, шлёт его ОДНИМ
    sendall(), читает ответ, сравнивает.

    Возвращает (pps, kbps, received, ok), как и в check_2_netcans_simple.py.
    """
    sent_frames = [(TEST_CAN_ID, random.randbytes(MSG_DATA_SIZE)) for _ in range(count)]
    payload = b"".join(_make_frame_ascii(cid, data) for cid, data in sent_frames)

    print(f"{NETCAN_HOST}:{PORT_CH0} -> {NETCAN_HOST}:{PORT_CH1}  "
          f"{count} кадров, {speed} Kbit/s (raw-сокеты, без vs_can_lib)")

    t0 = time.perf_counter()
    sock_send.sendall(payload)      # ОДИН системный вызов на весь burst
    recv_frames = _read_frames(sock_recv, count, timeout)
    t1 = time.perf_counter()

    # подтверждения Z\r от платы копятся в сокете отправителя -- выбрасываем,
    # чтобы не переполнить буфер приёма ПК за серию прогонов
    _drain_socket(sock_send)

    dt = t1 - t0
    received = len(recv_frames)
    pps = received / dt if dt > 0 else 0.0
    kbps = received * MSG_DATA_SIZE / 1024 / dt if dt > 0 else 0.0
    ok = received == count and recv_frames == sent_frames

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


def raw_find_max_throughput(speed=1000, counts=None, timeout=10):
    """
    Аналог test_find_max_throughput(), но на чистых сокетах, без vs_can_lib.
    FDCAN1 (порт 2001, отправка) -> FDCAN2 (порт 2002, приём).
    """
    counts = counts or [300, 320, 330, 350, 400, 500, 700, 1000]

    print(f"Поиск максимальной пропускной способности на {speed} Kbit/s "
          f"(RAW-СОКЕТЫ, без vs_can_lib/vs_can_api.dll)")
    print(f"Пробуем размеры пачки: {counts}")

    sock_send = _open_channel(NETCAN_HOST, PORT_CH0, speed)
    sock_recv = _open_channel(NETCAN_HOST, PORT_CH1, speed)
    # sendall под back-pressure может легально идти долго -- без таймаута
    sock_send.settimeout(None)

    results = []
    last_good = None
    try:
        for count in counts:
            print(f"########## count = {count} ##########")
            pps, kbps, received, ok = raw_load_pass(sock_send, sock_recv, speed, count, timeout)
            results.append((count, pps, kbps, received, ok))

            if ok:
                last_good = count
            else:
                print(f"<Стоп> Потери начались на count={count}, дальше не идём")
                break

            time.sleep(PAUSE_S)
    finally:
        _close_channel(sock_send)
        _close_channel(sock_recv)

    print("=" * 60)
    print(f"ИТОГ (raw-сокеты, поиск максимума, {speed} Kbit/s)")
    print("=" * 60)
    for count, pps, kbps, received, ok in results:
        status = "OK" if ok else "ОШИБКА"
        print(f"count={count:>5}   {pps:>8.1f} пакет/с   {kbps:>8.2f} КБ/с   "
              f"{received}/{count}   {status}")

    if last_good is None:
        print("\nДаже наименьшая пачка не прошла без потерь")
    elif last_good == counts[-1]:
        print(f"\nВсе пачки до count={last_good} прошли без потерь -- "
              f"предел ещё не найден, попробуйте увеличить counts")
    else:
        print(f"\nПрактический максимум без потерь (raw-сокеты): count={last_good}")
    print()


# ============================================================
#                  НЕПРЕРЫВНЫЙ ПОТОК
# ============================================================
def raw_continuous_test(speed=1000, duration=60, batch_size=300,
                        gap_warn_ms=5.0, drain_idle_s=2.0):
    """
    Непрерывный поток на чистых сокетах, без vs_can_lib/vs_can_api.dll.
    FDCAN1 (порт 2001) шлёт -> FDCAN2 (порт 2002) принимает.

    Три независимых потока:
      sender     -- непрерывно шлёт пачки по batch_size кадров через sendall(),
                    ничего не дожидаясь. Под back-pressure sendall блокируется --
                    это штатно (TCP-окно платы закрыто, шина занята).
      ack_reader -- вычитывает ответы платы в сокете ОТПРАВИТЕЛЯ: "Z\\r" на каждый
                    принятый кадр, "\\a" на отвергнутый. Без него буфер приёма ПК
                    переполнится и плата не сможет отдавать подтверждения.
      receiver   -- читает сокет получателя и проверяет каждый кадр:
                    байты 0-3 -- seq, байты 4-7 -- seq ^ 0xA5A5A5A5.

      duration     -- сколько секунд непрерывно слать
      batch_size   -- размер одной пачки (кадров на один sendall)
      gap_warn_ms  -- порог простоя Python между пачками (мс). Меряется от КОНЦА
                      одного sendall до НАЧАЛА следующего: время, пока sendall
                      заблокирован, паузой НЕ считается -- это back-pressure,
                      буферы полны, шина занята.
      drain_idle_s -- после остановки sender'а: сколько секунд без новых данных
                      ждать, прежде чем считать приём законченным
    """
    sock_send = _open_channel(NETCAN_HOST, PORT_CH0, speed)
    sock_recv = _open_channel(NETCAN_HOST, PORT_CH1, speed)
    # Блокирующие сокеты без таймаута: sendall под back-pressure может идти
    # сколь угодно долго, а чтение ведём через select, чтобы не зависать.
    sock_send.settimeout(None)
    sock_recv.settimeout(None)

    print(f"{NETCAN_HOST}:{PORT_CH0} -> {NETCAN_HOST}:{PORT_CH1}  непрерывный поток "
          f"{duration} с, пачками по {batch_size}, {speed} Kbit/s (raw-сокеты)")

    st = {
        "sent": 0, "acks": 0, "bells": 0,
        "received": 0, "corrupted": 0, "seq_breaks": 0, "lost": 0, "next_expected": 0,
        "gaps_detected": 0, "max_gap_ms": 0.0,
        "t_send_start": None, "t_send_end": None, "t_first_rx": None, "t_last_rx": None,
        "errors": [],
    }
    sender_done = threading.Event()
    abort = threading.Event()

    def sender():
        seq = 0
        prev_end = None
        st["t_send_start"] = time.perf_counter()
        try:
            while not abort.is_set() and (time.perf_counter() - st["t_send_start"]) < duration:
                payload = b"".join(
                    _make_frame_ascii(TEST_CAN_ID, _make_seq_data(s))
                    for s in range(seq, seq + batch_size)
                )

                t_before = time.perf_counter()
                if prev_end is not None:
                    gap_ms = (t_before - prev_end) * 1000.0
                    st["max_gap_ms"] = max(st["max_gap_ms"], gap_ms)
                    if gap_ms > gap_warn_ms:
                        st["gaps_detected"] += 1
                        print(f"<Пауза> простой Python между пачками {gap_ms:.2f} мс "
                              f"(порог {gap_warn_ms:.1f} мс), seq={seq}")

                sock_send.sendall(payload)
                prev_end = time.perf_counter()
                seq += batch_size
                st["sent"] = seq
        except Exception as e:
            st["errors"].append(f"sender: {e!r}")
            print(f"<Error> sender упал: {e!r} (успешно отправлено: {st['sent']})")
            abort.set()
        finally:
            st["t_send_end"] = time.perf_counter()
            sender_done.set()

    def ack_reader():
        last_rx = time.perf_counter()
        try:
            while not abort.is_set():
                ready, _, _ = select.select([sock_send], [], [], 0.05)
                now = time.perf_counter()
                if ready:
                    chunk = sock_send.recv(65536)
                    if not chunk:
                        raise ConnectionError("плата закрыла соединение отправителя")
                    st["acks"] += chunk.count(b"Z")
                    st["bells"] += chunk.count(b"\x07")
                    last_rx = now
                elif sender_done.is_set() and (st["acks"] + st["bells"] >= st["sent"]
                                               or now - last_rx > drain_idle_s):
                    break
        except Exception as e:
            st["errors"].append(f"ack_reader: {e!r}")
            print(f"<Error> ack_reader упал: {e!r}")
            abort.set()

    def receiver():
        buf = b""
        last_rx = time.perf_counter()
        try:
            while not abort.is_set():
                ready, _, _ = select.select([sock_recv], [], [], 0.05)
                now = time.perf_counter()
                if ready:
                    chunk = sock_recv.recv(65536)
                    if not chunk:
                        raise ConnectionError("плата закрыла соединение получателя")
                    last_rx = now
                    if st["t_first_rx"] is None:
                        st["t_first_rx"] = now
                    st["t_last_rx"] = now

                    frames, buf = _parse_frames(buf + chunk)
                    for can_id, data in frames:
                        if can_id != TEST_CAN_ID or len(data) != 8:
                            st["corrupted"] += 1
                            continue
                        seq, check = struct.unpack(">II", data)
                        if check != (seq ^ CHECK_XOR) & MASK32:
                            st["corrupted"] += 1
                            if st["corrupted"] <= 10:
                                print(f"<Error> битый кадр: id=0x{can_id:08X} data={data.hex()}")
                            continue
                        if seq != st["next_expected"]:
                            st["seq_breaks"] += 1
                            if seq > st["next_expected"]:
                                st["lost"] += seq - st["next_expected"]
                            print(f"<Error> разрыв: ожидали {st['next_expected']}, пришло {seq}")
                        st["next_expected"] = seq + 1
                        st["received"] += 1
                elif sender_done.is_set() and (st["received"] >= st["sent"]
                                               or now - last_rx > drain_idle_s):
                    break
        except Exception as e:
            st["errors"].append(f"receiver: {e!r}")
            print(f"<Error> receiver упал: {e!r} (принято к этому моменту: {st['received']})")
            abort.set()

    threads = [threading.Thread(target=receiver, name="receiver"),
               threading.Thread(target=ack_reader, name="ack_reader"),
               threading.Thread(target=sender, name="sender")]
    try:
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        print("\nОстановлено (Ctrl+C)")
        abort.set()
        for t in threads:
            t.join()
    finally:
        _close_channel(sock_send)
        _close_channel(sock_recv)

    # ---------- итог ----------
    send_dt = (st["t_send_end"] - st["t_send_start"]) if st["t_send_start"] is not None else 0.0
    rx_dt = (st["t_last_rx"] - st["t_first_rx"]) if st["t_first_rx"] is not None else 0.0
    send_pps = st["sent"] / send_dt if send_dt > 0 else 0.0
    rx_pps = st["received"] / rx_dt if rx_dt > 0 else 0.0

    print()
    print("=" * 60)
    print(f"ИТОГ (raw-сокеты, непрерывный поток, {speed} Kbit/s)")
    print("=" * 60)
    print(f"Отправлено:            {st['sent']} за {send_dt:.2f} с  ({send_pps:.1f} пакет/с)")
    print(f"Подтверждено платой:   Z={st['acks']}, отвергнуто (BELL)={st['bells']}")
    print(f"Принято на канале 2:   {st['received']} за {rx_dt:.2f} с  ({rx_pps:.1f} пакет/с, "
          f"потолок шины ~{BUS_CEILING_PPS} пакет/с при 1 Мбит/с)")
    print(f"Битых кадров: {st['corrupted']}")
    print(f"Разрывов последовательности: {st['seq_breaks']} (пропущено кадров: {st['lost']})")
    print(f"Простой Python между пачками: макс. {st['max_gap_ms']:.2f} мс, "
          f"превышений порога {gap_warn_ms:.1f} мс: {st['gaps_detected']}")

    if st["errors"]:
        print("<Error> тест завершился аварийно -- выводы недостоверны:")
        for err in st["errors"]:
            print(f"  - {err}")
        print()
        return

    ok = (st["received"] == st["sent"] and st["acks"] == st["sent"] and st["bells"] == 0
          and st["seq_breaks"] == 0 and st["corrupted"] == 0)
    if ok:
        print("OK -- все кадры подтверждены платой и дошли по порядку без искажений")
    else:
        print(f"<Error> недостача на приёме: {st['sent'] - st['received']}, "
              f"неподтверждённых: {st['sent'] - st['acks']}, BELL: {st['bells']}")
    print()


# ============================================================
#                           MAIN
# ============================================================
if __name__ == "__main__":
    # Burst: растим размер пачки до первых потерь
    # raw_find_max_throughput(speed=1000, counts=[300, 320, 330, 350, 400, 500, 700, 1000, 2000, 5000])

    # Непрерывный поток
    raw_continuous_test(speed=1000, duration=60, batch_size=300)
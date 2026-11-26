"""
Проверка сетевой карты и TCP-пути БЕЗ CAN (нужна прошивка с ETH_BENCH_MODE=1).

  ПК -> плата, порт 2001: непрерывный поток команд в SLCAN-подобном формате,
      27 байт на строку ("t" + 8 hex ID + "8" + 16 hex данных + "\\r").
      Плата их выбрасывает (CAN и парсер не участвуют), но RX-путь
      (TCP, кольцо, RX-пул, DMA) нагружен полностью.
  плата -> ПК, порт 2002: плата сама непрерывно шлёт строки
      "0000000123\\r" (10 цифр порядкового номера, 11 байт). ПК проверяет
      номера: пропуск номера = потерянная строка.

Почему сокеты, а не vs_can_lib:
  - строки ответа не являются CAN-кадрами, парсер vs_can_lib их не разберёт;
  - в bench-режиме плата не отвечает на служебные команды (открытие канала,
    set_speed), и библиотека может зависнуть в ожидании ответа.
  Флаг USE_VSCAN_SENDER здесь не реализован: отправка через vs_can_lib
  в этом режиме не нужна и не проверялась.

Это НЕ loopback: loopback в vs_can_lib -- один канал в режиме self-reception.
"""
import socket
import threading
import time

# ============================================================
#                        НАСТРОЙКИ
# ============================================================
HOST = "192.168.0.18"
PORT_CMD = 2001        # ПК -> плата (команды, выбрасываются)
PORT_RESP = 2002       # плата -> ПК (непрерывные строки ответа)

DURATION_S = 900      # сколько секунд гонять поток
PROGRESS_S = 30.0      # печатать прогресс раз в столько секунд
CMD_BATCH = 1000       # команд в одном sendall
RECV_CHUNK = 65536
DRAIN_S = 2.0          # после остановки: сколько ждать хвост ответов перед закрытием
TEST_CAN_ID = 0x10203040

# ============================================================
#                  ФОРМАТ СТРОК
# ============================================================
def make_cmd_line(seq):
    """Команда длиной ровно 27 байт, как SLCAN-кадр с 8 байтами данных."""
    data = (seq & 0xFFFFFFFF).to_bytes(4, "big") + ((seq ^ 0xA5A5A5A5) & 0xFFFFFFFF).to_bytes(4, "big")
    return f"t{TEST_CAN_ID:08X}8{data.hex().upper()}\r".encode("ascii")


# ============================================================
#                  ПОТОКИ
# ============================================================
def sender(sock, stop, st):
    """Шлёт команды непрерывно, пока не выйдет время или не будет stop."""
    payload = b"".join(make_cmd_line(i) for i in range(CMD_BATCH))  # один и тот же блок
    t0 = time.perf_counter()
    st["t_send0"] = t0
    try:
        while not stop.is_set() and (time.perf_counter() - t0) < DURATION_S:
            sock.sendall(payload)
            st["cmds"] += CMD_BATCH
    except OSError as e:
        st["sender_err"] = repr(e)
        print(f"<Error> sender: {e!r} (отправлено команд: {st['cmds']})")
    finally:
        st["t_send1"] = time.perf_counter()


def receiver(sock, stop, st):
    """Читает строки ответа платы, проверяет порядок номеров."""
    buf = b""
    expected = None
    last = time.perf_counter()
    try:
        while not stop.is_set():
            chunk = sock.recv(RECV_CHUNK)
            if not chunk:
                st["recv_err"] = "плата закрыла соединение"
                break
            now = time.perf_counter()
            gap_ms = (now - last) * 1000.0
            if gap_ms > st["max_gap_ms"]:
                st["max_gap_ms"] = gap_ms
            last = now

            buf += chunk
            *lines, buf = buf.split(b"\r")
            for ln in lines:
                st["lines"] += 1
                if len(ln) != 10 or not ln.isdigit():
                    st["bad"] += 1
                    continue
                n = int(ln)
                if expected is not None and n != expected:
                    st["breaks"] += 1
                    if n > expected:
                        st["missing"] += n - expected
                    if st["breaks"] <= 10:
                        print(f"<Error> разрыв номеров: ожидали {expected}, пришло {n}")
                expected = n + 1
    except OSError as e:
        st["recv_err"] = repr(e)
        print(f"<Error> receiver: {e!r}")


# ============================================================
#                  ПРОГРЕСС И ЗАПУСК
# ============================================================
def print_progress(st, t_start, prev):
    elapsed = time.perf_counter() - t_start
    dt = elapsed - prev["t"]
    cmd_rate = (st["cmds"] - prev["cmds"]) / dt if dt > 0 else 0.0
    line_rate = (st["lines"] - prev["lines"]) / dt if dt > 0 else 0.0
    print(f"[{time.strftime('%H:%M:%S')}] t={elapsed:7.1f} с | "
          f"команд ПК->плата: {cmd_rate:8.0f}/с | "
          f"ответов плата->ПК: {line_rate:8.0f}/с | "
          f"пропущено: {st['missing']} (разрывов {st['breaks']}) | "
          f"битых: {st['bad']} | макс. пауза приёма: {st['max_gap_ms']:.1f} мс")
    st["max_gap_ms"] = 0.0
    prev.update(t=elapsed, cmds=st["cmds"], lines=st["lines"])


def main():
    st = {
        "cmds": 0, "lines": 0, "bad": 0, "breaks": 0, "missing": 0,
        "max_gap_ms": 0.0, "sender_err": None, "recv_err": None,
        "t_send0": None, "t_send1": None,
    }

    print(f"Bench-проверка: команды ПК->{HOST}:{PORT_CMD}, ответы {HOST}:{PORT_RESP}, "
          f"{DURATION_S} с, прогресс каждые {PROGRESS_S:.0f} с")

    # Сначала приёмник: плата начинает генерировать ответы при подключении на порт 2002.
    recv_sock = socket.create_connection((HOST, PORT_RESP))
    recv_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    cmd_sock = socket.create_connection((HOST, PORT_CMD))
    cmd_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    stop = threading.Event()
    t_recv = threading.Thread(target=receiver, args=(recv_sock, stop, st), daemon=True)
    t_send = threading.Thread(target=sender, args=(cmd_sock, stop, st), daemon=True)

    t_start = time.perf_counter()
    prev = {"t": 0.0, "cmds": 0, "lines": 0}
    t_recv.start()
    t_send.start()

    try:
        while t_send.is_alive() and not (st["sender_err"] or st["recv_err"]):
            time.sleep(PROGRESS_S)
            print_progress(st, t_start, prev)
    except KeyboardInterrupt:
        print("\nОстановлено (Ctrl+C)")

    # Остановка: отправитель уже вышел по времени; даём приёмнику дочитать хвост.
    stop.set()
    t_send.join(timeout=5)
    time.sleep(DRAIN_S)
    try:
        recv_sock.shutdown(socket.SHUT_RDWR)   # разблокирует recv()
    except OSError:
        pass
    recv_sock.close()
    cmd_sock.close()
    t_recv.join(timeout=5)

    # ---------- итог ----------
    send_dt = (st["t_send1"] - st["t_send0"]) if st["t_send0"] else 0.0
    cmd_rate = st["cmds"] / send_dt if send_dt > 0 else 0.0
    print()
    print("=" * 60)
    print("ИТОГ (bench, без CAN)")
    print("=" * 60)
    print(f"Команд отправлено ПК:   {st['cmds']} за {send_dt:.1f} с  ({cmd_rate:.0f}/с)")
    print(f"Строк ответа принято:   {st['lines']}")
    print(f"Пропущено строк:        {st['missing']} (разрывов номеров: {st['breaks']})")
    print(f"Битых строк:            {st['bad']}")
    if st["sender_err"]:
        print(f"<Error> отправитель упал: {st['sender_err']}")
    if st["recv_err"]:
        print(f"<Error> приёмник упал: {st['recv_err']}")
    print()
    print("Потери на сетевой карте ПК смотрите отдельно: Get-NetAdapterStatistics")
    print("до и после прогона (ReceivedDiscardedPackets, ReceivedPacketErrors).")
    print("На плате -- строка [CLIENT] ... BENCH cmds_discarded / lines_sent, "
          "и [ETH-STAT] / [EVLOG] при disconnect.")


if __name__ == "__main__":
    main()

"""QBT Camera self-test: drives every feature of the iPhone app over the stream protocol and checks
that video keeps flowing after each step.

    python phone_selftest.py 192.168.1.5        (close QBTCamera.exe first: the phone serves one client)

For each step it reports the frame rate after the change settled and the longest gap between
frames since the command (how long the picture froze), plus a PASS/FAIL verdict.
"""
import json
import socket
import struct
import sys
import threading
import time

HEADER = struct.Struct("<IBBHIQ")
ACK_MAGIC, COMMAND_MAGIC = 0x4B544251, 0x43544251
TYPE_VIDEO, TYPE_MESSAGE, TYPE_AUDIO = 1, 2, 3


class Phone:
    def __init__(self, ip):
        self.sock = socket.create_connection((ip, 50500), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.lock = threading.Lock()
        self.send_lock = threading.Lock()  # acks (reader thread) and commands must not interleave
        self.frames = []   # (time, size, keyframe)
        self.audio = []    # arrival times
        self.state = {}
        self.caps = {}
        self.caps_times = []
        self.error = None
        threading.Thread(target=self._reader, daemon=True).start()

    def _recv(self, n):
        data = bytearray()
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("phone closed the connection")
            data += chunk
        return bytes(data)

    def _reader(self):
        received = 0
        try:
            while True:
                _, kind, flags, _, length, _ = HEADER.unpack(self._recv(HEADER.size))
                payload = self._recv(length)
                now = time.monotonic()
                if kind == TYPE_VIDEO:
                    received += 1
                    with self.lock:
                        self.frames.append((now, length, flags & 1))
                    with self.send_lock:
                        self.sock.sendall(struct.pack("<II", ACK_MAGIC, received & 0xFFFFFFFF))
                elif kind == TYPE_MESSAGE:
                    message = json.loads(payload)
                    with self.lock:
                        if message.get("evt") == "state":
                            self.state = message
                        elif message.get("evt") == "caps":
                            self.caps = message
                            self.caps_times.append(now)
                elif kind == TYPE_AUDIO:
                    with self.lock:
                        self.audio.append(now)
        except Exception as error:  # noqa: BLE001 - report whatever killed the link
            self.error = error

    def send(self, **command):
        body = json.dumps(command).encode()
        with self.send_lock:
            self.sock.sendall(struct.pack("<II", COMMAND_MAGIC, len(body)) + body)

    def frames_between(self, start, end):
        with self.lock:
            return [f for f in self.frames if start <= f[0] < end]

    def audio_between(self, start, end):
        with self.lock:
            return [t for t in self.audio if start <= t < end]


results = []


def step(phone, name, commands, expected_fps, settle=1.5, measure=2.0, max_freeze_ms=1200):
    started = time.monotonic()
    for command in commands:
        phone.send(**command)
    time.sleep(settle + measure)
    if phone.error:
        raise SystemExit(f"LINK LOST during '{name}': {phone.error}")
    window = phone.frames_between(started + settle, started + settle + measure)
    fps = len(window) / measure
    since = phone.frames_between(started - 0.5, started + settle + measure)
    times = [started] + [f[0] for f in since if f[0] >= started]
    freeze = max((b - a for a, b in zip(times, times[1:])), default=settle + measure) * 1000
    ok = fps >= expected_fps * 0.8 and freeze <= max_freeze_ms
    results.append((name, fps, freeze, ok))
    effects_ms = phone.state.get("effectsMs", 0)
    print(f"{'PASS' if ok else 'FAIL'}  {name:42} {fps:5.1f} к/с  замирание {freeze:5.0f} мс"
          f"{f'  эффекты {effects_ms:.1f} мс' if effects_ms else ''}", flush=True)
    return ok


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    phone = Phone(sys.argv[1] if len(sys.argv) > 1 else "192.168.1.5")
    for _ in range(50):  # capabilities arrive right after connecting
        if phone.caps:
            break
        time.sleep(0.1)
    time.sleep(1)
    lenses = phone.caps.get("lenses", "wide").split(",")
    print(f"Подключено. Линзы: {lenses}\n")
    no_effects = dict(cmd="effects", blur=0, retouch=0, framing=False, filter="none", filterAmount=1, mirror=False)
    base = [no_effects, dict(cmd="orientation", value="auto"), dict(cmd="lens", value="wide")]

    step(phone, "исходное: 1080p 30, 1×", base + [dict(cmd="preset", resolution="fullHD", fps=30)], 30, settle=2.5)

    for resolution, fps in [("hd720", 30), ("fullHD", 30), ("fullHD", 60), ("uhd4K", 30), ("uhd4K", 60)]:
        step(phone, f"качество {resolution} {fps}", [dict(cmd="preset", resolution=resolution, fps=fps)], fps,
             max_freeze_ms=2000)
        for lens in lenses:
            step(phone, f"  {resolution} {fps}: линза {lens}", [dict(cmd="lens", value=lens)], fps, max_freeze_ms=2000)
        phone.send(cmd="lens", value="wide")
        time.sleep(1.5)

    step(phone, "обратно 1080p 30", [dict(cmd="preset", resolution="fullHD", fps=30)], 30, max_freeze_ms=2000)
    for orientation in ["landscape", "portrait", "landscapeFlipped", "auto"]:
        step(phone, f"поворот {orientation}", [dict(cmd="orientation", value=orientation)], 30)

    effect_steps = [
        ("зеркало", dict(mirror=True)),
        ("размытие фона", dict(blur=0.8)),
        ("ретушь", dict(retouch=0.6)),
        ("слежение за лицом", dict(framing=True)),
        ("фильтр кино", dict(filter="cinema")),
        ("фильтр ч/б", dict(filter="mono")),
        ("фильтр мягкий", dict(filter="soft")),
        ("всё сразу", dict(blur=0.8, retouch=0.5, framing=True, filter="warm", mirror=True)),
    ]
    for name, settings in effect_steps:
        step(phone, f"эффект: {name}", [{**no_effects, **settings}], 30)
    step(phone, "всё сразу + поворот", [dict(cmd="orientation", value="landscape")], 30)
    step(phone, "всё сразу + фронтальная", [dict(cmd="lens", value="front")], 30, max_freeze_ms=2000)
    step(phone, "всё сразу в 4K 30", [dict(cmd="preset", resolution="uhd4K", fps=30)], 30, max_freeze_ms=2000)
    step(phone, "эффекты выкл, 1080p 30, 1×",
         [no_effects, dict(cmd="orientation", value="auto"), dict(cmd="lens", value="wide"),
          dict(cmd="preset", resolution="fullHD", fps=30)], 30, settle=2.5, max_freeze_ms=2500)

    step(phone, "микрофон выкл", [dict(cmd="audio", enabled=False)], 30)
    started = time.monotonic()
    step(phone, "микрофон вкл", [dict(cmd="audio", enabled=True)], 30)
    audio = len(phone.audio_between(started + 1.5, started + 3.5))
    print(f"      звук после включения: {audio} пакетов за 2 с ({'есть' if audio > 40 else 'НЕТ'})")
    results.append(("звук идёт после включения", audio / 2, 0, audio > 40))

    step(phone, "затемнение экрана вкл", [dict(cmd="screen", blackout=True)], 30)
    step(phone, "затемнение экрана выкл", [dict(cmd="screen", blackout=False)], 30)
    step(phone, "суфлёр показать", [dict(cmd="prompter", visible=True, playing=True, speed=3)], 30)
    step(phone, "суфлёр спрятать", [dict(cmd="prompter", visible=False)], 30)
    step(phone, "зум 3×", [dict(cmd="zoom", value=3.0)], 30)
    step(phone, "зум 1×", [dict(cmd="zoom", value=1.0)], 30)
    step(phone, "фонарик вкл", [dict(cmd="torch", value=0.5)], 30)
    step(phone, "фонарик выкл", [dict(cmd="torch", value=0)], 30)
    step(phone, "ручная экспозиция", [dict(cmd="exposureManual", iso=400, shutterUs=10000)], 30)
    step(phone, "авто экспозиция", [dict(cmd="exposureAuto")], 30)

    burst = [dict(cmd="lens", value=lens) for lens in lenses * 2] + [dict(cmd="lens", value="wide")]
    step(phone, "стресс: 9 переключений линз подряд", burst, 30, settle=3, max_freeze_ms=4000)
    burst = [dict(cmd="orientation", value=o) for o in ["landscape", "portrait", "landscapeFlipped", "auto"] * 2]
    step(phone, "стресс: 8 поворотов подряд", burst, 30, settle=2.5, max_freeze_ms=3000)
    for index in range(20):  # the black screen toggled on and off quickly, as a user taps
        phone.send(cmd="screen", blackout=index % 2 == 0)
        time.sleep(0.12)
    step(phone, "стресс: 20 переключений чёрного экрана", [dict(cmd="screen", blackout=False)], 30,
         settle=1.5, max_freeze_ms=1200)

    failed = [r for r in results if not r[3]]
    print(f"\nИтого: {len(results) - len(failed)} из {len(results)} шагов прошли.")
    for name, fps, freeze, _ in failed:
        print(f"  НЕ ПРОШЁЛ: {name} ({fps:.1f} к/с, замирание {freeze:.0f} мс)")


if __name__ == "__main__":
    main()

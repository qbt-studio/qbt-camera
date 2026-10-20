"""QBT Camera viewer (stage 1).

Receives the iPhone stream over USB (Apple Mobile Device Service / usbmux) or Wi-Fi
and shows it in ffplay. Reconnects automatically.

    python qbt_viewer.py              # USB first, then search Wi-Fi
    python qbt_viewer.py --wifi IP    # connect to a known address
    python qbt_viewer.py --usb-only
"""
import argparse
import ipaddress
import plistlib
import shutil
import socket
import struct
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

VIDEO_PORT = 50500
MAGIC = 0x56544251  # 'QBTV'
HEADER = struct.Struct("<IBBHIQ")  # magic, type, flags, reserved, length, pts_us
TYPE_VIDEO = 1
FLAG_KEYFRAME = 1
# Sent back after every received frame: the phone limits unacknowledged frames,
# so video can never pile up in network buffers.
ACK = struct.Struct("<II")  # magic 'QBTK', total frames received on this connection
ACK_MAGIC = 0x4B544251
USBMUX_ADDRESS = ("127.0.0.1", 27015)
# Access unit delimiter after every frame: lets ffplay's parser finish the frame
# right away instead of waiting for the next one (saves a whole frame of latency).
ACCESS_UNIT_DELIMITER = b"\x00\x00\x00\x01\x09\xf0"


def recv_exact(sock, size):
    buffer = bytearray(size)
    view = memoryview(buffer)
    received = 0
    while received < size:
        count = sock.recv_into(view[received:], size - received)
        if count == 0:
            raise ConnectionError("iPhone закрыл соединение")
        received += count
    return buffer


# --- USB (usbmux protocol of Apple Mobile Device Service) ---------------------

def usbmux_request(sock, message, tag):
    body = plistlib.dumps({
        **message,
        "ClientVersionString": "qbtcam-viewer",
        "ProgName": "QBTCamViewer",
        "kLibUSBMuxVersion": 3,
    })
    sock.sendall(struct.pack("<IIII", 16 + len(body), 1, 8, tag) + body)
    length = struct.unpack("<IIII", recv_exact(sock, 16))[0]
    return plistlib.loads(bytes(recv_exact(sock, length - 16)))


def usb_devices():
    with socket.create_connection(USBMUX_ADDRESS, timeout=2) as sock:
        reply = usbmux_request(sock, {"MessageType": "ListDevices"}, 1)
    return [d for d in reply.get("DeviceList", [])
            if d.get("Properties", {}).get("ConnectionType") == "USB"]


def usb_connect(device_id, port):
    sock = socket.create_connection(USBMUX_ADDRESS, timeout=3)
    reply = usbmux_request(sock, {
        "MessageType": "Connect",
        "DeviceID": device_id,
        "PortNumber": socket.htons(port),  # usbmux wants network byte order
    }, 2)
    if reply.get("Number") != 0:
        sock.close()
        raise ConnectionError(f"usbmux отказал (код {reply.get('Number')}): приложение на iPhone запущено?")
    return sock  # from here on the socket is a raw tunnel to the app


# --- Wi-Fi ----------------------------------------------------------------------

def local_subnet():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect(("10.255.255.255", 1))  # no packet is sent; just picks the LAN interface
        address = probe.getsockname()[0]
    return ipaddress.ip_network(f"{address}/24", strict=False), address


def is_qbt_camera(ip):
    """The app sends a keyframe right after a client connects, so a valid header proves it is us."""
    try:
        with socket.create_connection((ip, VIDEO_PORT), timeout=0.4) as sock:
            sock.settimeout(2)
            return HEADER.unpack(recv_exact(sock, HEADER.size))[0] == MAGIC
    except (OSError, ConnectionError, struct.error):
        return False


def find_on_wifi():
    subnet, own_address = local_subnet()
    hosts = [str(h) for h in subnet.hosts() if str(h) != own_address]
    with ThreadPoolExecutor(max_workers=64) as pool:
        for ip, found in zip(hosts, pool.map(is_qbt_camera, hosts)):
            if found:
                return ip
    return None


# --- Main loop ------------------------------------------------------------------

def connect(args, announce):
    if not args.wifi:
        try:
            for device in usb_devices():
                serial = device.get("Properties", {}).get("SerialNumber", "?")
                try:
                    return usb_connect(device["DeviceID"], VIDEO_PORT), f"USB ({serial})"
                except (OSError, ConnectionError) as error:
                    announce(f"USB: {error}")
        except OSError:
            announce("USB недоступен: служба Apple Mobile Device Service не отвечает")
    if args.usb_only:
        return None, None

    ip = args.wifi
    if not ip:
        announce("Ищу iPhone в сети Wi-Fi…")
        ip = find_on_wifi()
    if ip:
        try:
            return socket.create_connection((ip, VIDEO_PORT), timeout=3), f"Wi-Fi ({ip})"
        except OSError as error:
            announce(f"Wi-Fi {ip}: {error}")
    return None, None


def start_player(ffplay):
    return subprocess.Popen([
        ffplay, "-hide_banner", "-loglevel", "error",
        "-window_title", "QBT Camera",
        "-y", "900",  # fit 4K/portrait video on screen; aspect ratio is kept, window is resizable
        "-fflags", "nobuffer", "-flags", "low_delay", "-framedrop",
        "-probesize", "32", "-analyzeduration", "0",
        # Raw H.264 has no timestamps and ffplay would pace it at 25 fps, queueing every
        # extra frame (seconds of lag after a minute). setpts=0 shows each frame on arrival.
        "-vf", "setpts=0",
        "-f", "h264", "-i", "-",
    ], stdin=subprocess.PIPE)


def stream(sock, player):
    sock.settimeout(5)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    frames = keyframes = byte_count = 0
    received = 0
    window_start = time.monotonic()
    while True:
        magic, packet_type, flags, _, length, _pts = HEADER.unpack(recv_exact(sock, HEADER.size))
        if magic != MAGIC:
            raise ConnectionError("неожиданные данные в потоке")
        payload = recv_exact(sock, length)
        if packet_type != TYPE_VIDEO:
            continue
        player.stdin.write(payload)
        player.stdin.write(ACCESS_UNIT_DELIMITER)
        player.stdin.flush()
        # Acknowledge only after the player took the frame: if it falls behind, the phone slows down.
        received += 1
        sock.sendall(ACK.pack(ACK_MAGIC, received & 0xFFFFFFFF))

        frames += 1
        keyframes += flags & FLAG_KEYFRAME
        byte_count += HEADER.size + length
        elapsed = time.monotonic() - window_start
        if elapsed >= 1:
            print(f"\r  {frames / elapsed:5.1f} к/с   {byte_count * 8 / elapsed / 1e6:5.1f} Мбит/с   "
                  f"ключевых кадров: {keyframes}   ", end="", flush=True)
            frames = keyframes = byte_count = 0
            window_start = time.monotonic()


def main():
    parser = argparse.ArgumentParser(description="QBT Camera: просмотр видео с iPhone")
    parser.add_argument("--wifi", metavar="IP", help="адрес iPhone в Wi-Fi (как показано в приложении)")
    parser.add_argument("--usb-only", action="store_true", help="только USB-кабель")
    args = parser.parse_args()

    ffplay = shutil.which("ffplay")
    if not ffplay:
        sys.exit("Не найден ffplay. Установите FFmpeg: winget install Gyan.FFmpeg")

    player = start_player(ffplay)
    last_message = None

    def announce(message):
        nonlocal last_message
        if message != last_message:
            print(message)
            last_message = message

    try:
        while player.poll() is None:
            sock, route = connect(args, announce)
            if sock is None:
                announce("iPhone не найден. Откройте QBT Camera на телефоне. Повтор через 2 с…")
                time.sleep(2)
                continue
            announce(f"Подключено через {route}")
            try:
                stream(sock, player)
            except (OSError, ConnectionError) as error:
                if player.poll() is not None:
                    break
                print()
                announce(f"Связь потеряна ({error}). Переподключаюсь…")
            finally:
                sock.close()
    except KeyboardInterrupt:
        pass
    finally:
        if player.poll() is None:
            player.kill()


if __name__ == "__main__":
    main()

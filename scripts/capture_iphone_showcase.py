import os
import subprocess
import time
import socket
import struct
import plistlib
import json
from PIL import Image

SSH_CONFIG = os.path.expanduser("~/.ssh/config").replace("\\", "/")
USBMUX_ADDRESS = ('127.0.0.1', 27015)
DEVICE_UDID = "00008120-00181D063A98201E"
COMMAND_MAGIC = 0x43544251

def recv_exact(sock, size):
    buf = bytearray(size)
    view = memoryview(buf)
    rec = 0
    while rec < size:
        c = sock.recv_into(view[rec:], size - rec)
        if c == 0: raise ConnectionError('closed')
        rec += c
    return bytes(buf)

def send_phone_command(**cmd):
    sock = socket.create_connection(USBMUX_ADDRESS, timeout=3)
    body = plistlib.dumps({'MessageType': 'Connect', 'DeviceID': 2, 'PortNumber': socket.htons(50500), 'ProgName': 'qbt', 'ClientVersionString': '1.0', 'kLibUSBMuxVersion': 3})
    sock.sendall(struct.pack('<IIII', 16 + len(body), 1, 8, 1) + body)
    length = struct.unpack('<IIII', recv_exact(sock, 16))[0]
    plistlib.loads(recv_exact(sock, length - 16))
    
    b = json.dumps(cmd).encode()
    sock.sendall(struct.pack('<II', COMMAND_MAGIC, len(b)) + b)
    time.sleep(0.3)
    sock.close()

def capture_screenshot(remote_name, local_path):
    remote_path = f"/tmp/{remote_name}.png"
    cmd_cap = f'ssh -F "{SSH_CONFIG}" qbtmac "xcrun devicectl device capture screenshot --device {DEVICE_UDID} --destination {remote_path}"'
    subprocess.run(cmd_cap, shell=True, check=True)
    cmd_scp = f'scp -F "{SSH_CONFIG}" qbtmac:{remote_path} "{local_path}"'
    subprocess.run(cmd_scp, shell=True, check=True)
    
    # Resize to standard showcase 645x1398
    img = Image.open(local_path)
    if img.size == (1290, 2796):
        img_resized = img.resize((645, 1398), Image.Resampling.LANCZOS)
        img_resized.save(local_path)
        print(f"Saved {local_path} at 645x1398")
    else:
        print(f"Saved {local_path} at {img.size}")

def main():
    dest_dir = r"C:\FILES\QBTCameraStudio\docs\screenshots"
    
    # 1. Background
    print("--- 1. Capturing Background Tab ---")
    send_phone_command(cmd="tab", value="background")
    send_phone_command(cmd="prompter", visible=False)
    time.sleep(1.0)
    capture_screenshot("cap_background", f"{dest_dir}\\iphone-background.png")
    
    # 2. Exposure
    print("--- 2. Capturing Exposure Tab ---")
    send_phone_command(cmd="tab", value="exposure")
    time.sleep(1.0)
    capture_screenshot("cap_exposure", f"{dest_dir}\\iphone-exposure.png")
    
    # 3. Color
    print("--- 3. Capturing Color Tab ---")
    send_phone_command(cmd="tab", value="color")
    send_phone_command(cmd="effects", filter="warm", filterAmount=0.75, retouch=0.4)
    time.sleep(1.0)
    capture_screenshot("cap_color", f"{dest_dir}\\iphone-color.png")
    
    # 4. Stream
    print("--- 4. Capturing Stream Tab ---")
    send_phone_command(cmd="tab", value="stream")
    time.sleep(1.0)
    capture_screenshot("cap_stream", f"{dest_dir}\\iphone-stream.png")
    
    # 5. Prompter
    print("--- 5. Capturing Teleprompter ---")
    send_phone_command(cmd="prompter", visible=True, text="Welcome to QBT Camera. Looking directly into the lens while reading your notes, speech, or presentation script.")
    time.sleep(1.0)
    capture_screenshot("cap_prompter", f"{dest_dir}\\iphone-prompter.png")
    
    # Reset prompter back
    send_phone_command(cmd="prompter", visible=False)
    send_phone_command(cmd="tab", value="background")
    print("Done capturing all iPhone screenshots!")

if __name__ == "__main__":
    main()

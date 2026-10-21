import os
import sys
import json
import urllib.request
import urllib.parse

TOKEN = "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"
REPO = "qbt-studio/qbt-camera"
TAG = "v1.0"
TITLE = "QBT Camera 1.0"

BODY = """## QBT Camera 1.0 — Initial Release

Transform your iPhone into a professional webcam for Windows with ultra-low latency, real depth blur (LiDAR / TrueDepth), and full GPU compositing on your PC.

### 🌟 Features:
- 📷 **Pro Camera**: 720p / 1080p / 4K up to 60 fps, manual exposure (ISO, shutter stops, kelvin WB, manual focus).
- 🌫️ **Real Depth Portrait Blur**: True depth-based bokeh using iPhone LiDAR and TrueDepth sensors.
- 🖼️ **Background Replacement & Chroma Key**: Studio backdrops, custom images, and green screen with spill removal.
- 🎨 **Cinematic Looks & 3D LUT**: 8 built-in color looks and support for `.cube` LUT files on GPU.
- 🙂 **Face Smoothing & Tracking**: Intelligent skin smoothing and automatic framing.
- 📜 **Teleprompter**: Displays script notes right next to the front camera lens.
- 🔋 **Black Screen Mode**: Keeps iPhone cool and streaming without heating up the display.
- 🔌 **USB & Wi-Fi**: Instant automatic discovery and ultra-low latency.

---

### 📦 Files included in this release:

1. **`QBTCamera-Windows-1.0.zip`**: Complete Windows package (Application + Virtual Camera Driver + One-click Installer + User Manual).
2. **`QBTCamera.exe`**: Standalone Windows executable.
3. **`QBTCamera.ipa`**: iOS application package for iPhone (install via AltStore, Sideloadly, TrollStore, or Xcode).
"""

def create_release():
    url = f"https://api.github.com/repos/{REPO}/releases"
    headers = {
        "Authorization": f"Bearer {TOKEN}",
        "User-Agent": "qbt-release-tool",
        "Accept": "application/vnd.github+json"
    }
    payload = json.dumps({
        "tag_name": TAG,
        "target_commitish": "main",
        "name": TITLE,
        "body": BODY,
        "draft": False,
        "prerelease": False
    }).encode("utf-8")
    
    req = urllib.request.Request(url, data=payload, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req) as resp:
            data = json.loads(resp.read().decode())
            print(f"Created release ID: {data['id']}, URL: {data['html_url']}")
            return data
    except urllib.error.HTTPError as e:
        error_body = e.read().decode()
        print(f"HTTP Error {e.code}: {error_body}")
        # If release already exists, fetch it
        req_get = urllib.request.Request(f"https://api.github.com/repos/{REPO}/releases/tags/{TAG}", headers=headers)
        with urllib.request.urlopen(req_get) as resp_get:
            return json.loads(resp_get.read().decode())

def upload_asset(upload_url_template, file_path):
    filename = os.path.basename(file_path)
    upload_url = upload_url_template.split("{")[0] + f"?name={urllib.parse.quote(filename)}"
    print(f"Uploading {filename} ({os.path.getsize(file_path)} bytes)...")
    
    with open(file_path, "rb") as f:
        data = f.read()
    
    headers = {
        "Authorization": f"Bearer {TOKEN}",
        "User-Agent": "qbt-release-tool",
        "Content-Type": "application/octet-stream",
        "Content-Length": str(len(data))
    }
    
    req = urllib.request.Request(upload_url, data=data, headers=headers, method="POST")
    with urllib.request.urlopen(req) as resp:
        res = json.loads(resp.read().decode())
        print(f"Successfully uploaded {filename}! Download URL: {res.get('browser_download_url')}")

def main():
    rel = create_release()
    upload_url = rel["upload_url"]
    
    assets = [
        r"C:\FILES\QBTCameraStudio\release\QBTCamera-Windows-1.0.zip",
        r"C:\FILES\QBTCameraStudio\release\QBTCamera.exe",
        r"C:\FILES\QBTCameraStudio\release\QBTCamera.ipa"
    ]
    
    for asset in assets:
        if os.path.exists(asset):
            upload_asset(upload_url, asset)
        else:
            print(f"WARNING: File {asset} not found!")
    
    print("\nAll assets successfully published to GitHub Releases!")

if __name__ == "__main__":
    main()

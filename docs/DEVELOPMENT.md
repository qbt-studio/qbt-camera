# QBT Camera — Development

## Repository layout

| Path | What |
|---|---|
| `ios/` | iPhone app (SwiftUI, AVFoundation, VideoToolbox, Vision, Core Image + Metal kernels). iOS 18+. |
| `windows/host/` | Windows app `QBTCamera.exe`: receiver, GPU decode, Direct2D/DirectComposition UI, background compositor (D3D11 compute), audio, tray. |
| `windows/vcam/` | `QBTVirtualCamera.dll`, the Media Foundation virtual camera loaded by the Windows Frame Server. |
| `windows/common/` | Wire protocol and shared-memory frame exchange between the app and the camera DLL. |
| `windows/package/` | End-user installer (`Install QBT Camera.cmd`). |
| `windows/tools/` | Developer tools: stream viewer and the iPhone self-test. |
| `scripts/` | iPhone build/deploy over SSH to a Mac, Windows release packaging. |
| `docs/` | Manual, screenshots, publishing guide. |

## Build — Windows

Requirements: Visual Studio 2022 (or Build Tools) with “Desktop development with C++”, Windows SDK.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\make_release.ps1 -Version 1.0
```

Produces `release\QBTCamera-Windows-1.0.zip` (app, camera DLL, installer, manual). For development,
`windows\install_camera.ps1` registers the DLL straight from `windows\build\Release`.

## Build — iPhone

Requirements: a Mac with Xcode 26 or later, an Apple ID (free is enough for your own device), an
iPhone on iOS 18+.

```bash
bash scripts/ios_deploy.sh build    # syncs ios/ to the Mac and builds there
bash scripts/ios_deploy.sh install
bash scripts/ios_deploy.sh run
```

The script expects an SSH host alias `qbtmac` and the build agent from `ios/Tools/build_agent.sh`
running in the Mac’s GUI session (code signing needs the login keychain). Set `QBT_DEVICE` to the
target iPhone’s UDID. On the Mac itself you can simply open `ios/QBTCamera.xcodeproj` and press Run.

App Store / TestFlight: Xcode → Product → Archive with a paid Apple Developer Program membership.

## Protocol (TCP 50500, Bonjour `_qbtcam._tcp`, or USB via usbmux)

Phone → PC packets: 20-byte little-endian header `{magic 'QBTV', type, flags, length, ptsMicros}`.

| Type | Payload |
|---|---|
| 1 | H.264 Annex B access unit (low-latency, no B-frames) |
| 2 | JSON message: `{"evt":"caps"}` capabilities, `{"evt":"state"}` live state |
| 3 | 48 kHz mono s16le audio |
| 4 | H.264 matte stream `[foreground | blur amount]`, same pts as the picture (PC-side background) |

PC → phone: 8-byte `{magic, value}` — `'QBTK'` frame acks (flow control) or `'QBTC'` + JSON command
of `value` bytes (`lens`, `exposureManual`, `effects`, `preset`, `prompter`, …).

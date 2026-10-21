# QBT Camera — User Manual

QBT Camera turns your iPhone into a professional webcam for Windows. The picture appears as a camera
called **“QBT Camera”** in every app — Zoom, Microsoft Teams, Google Meet, Discord, OBS, browsers —
with very low latency over USB or Wi‑Fi, real depth-based background effects and full manual control
of the iPhone camera from your computer.

Everything stays on your devices: no account, no cloud, no watermark.

---

## Contents

1. [What you need](#1-what-you-need)
2. [Install on Windows](#2-install-on-windows)
3. [Install on iPhone](#3-install-on-iphone)
4. [First connection](#4-first-connection)
5. [Use QBT Camera in your apps](#5-use-qbt-camera-in-your-apps)
6. [The Windows app](#6-the-windows-app)
7. [Background effects](#7-background-effects)
8. [Color: filters and LUTs](#8-color-filters-and-luts)
9. [Audio](#9-audio)
10. [Stream, teleprompter and iPhone screen](#10-stream-teleprompter-and-iphone-screen)
11. [The iPhone app](#11-the-iphone-app)
12. [Troubleshooting](#12-troubleshooting)
13. [Uninstall](#13-uninstall)

---

## 1. What you need

| | Requirement |
|---|---|
| **iPhone** | iOS 17 or later. Best results: an iPhone with LiDAR (iPhone 12 Pro and later Pro models) for the main camera and Face ID (TrueDepth) for the front camera — they give real depth for background effects. |
| **Computer** | Windows 11 or Windows 10 (64‑bit) with a DirectX 11 graphics card that decodes H.264 in hardware (practically any GPU from the last 10 years). |
| **Connection** | The same Wi‑Fi network for both devices, **or** a USB cable. For USB, Apple’s device driver must be installed: install **Apple Devices** from the Microsoft Store (or iTunes). |

## 2. Install on Windows

1. Unzip `QBTCamera-Windows-1.0.zip` anywhere (for example to your Downloads folder).
2. Double-click **`Install QBT Camera.cmd`**.
3. Windows asks for permission (“Do you want to allow this app to make changes?”) — click **Yes**.
   The camera driver part has to be registered for all apps, which needs administrator rights once.
4. QBT Camera starts automatically. You will also find it in the Start menu as **QBT Camera**.

To update to a new version, run `Install QBT Camera.cmd` from the new package — your settings stay.

## 3. Install on iPhone

Open QBT Camera on the iPhone once installed (App Store / TestFlight build, or a developer build
installed from a Mac). On first launch allow:

- **Camera** — to send the picture.
- **Microphone** — to send sound (optional).
- **Local Network** — so the computer can find the iPhone over Wi‑Fi.

## 4. First connection

1. Start **QBT Camera on the computer**.
2. Open **QBT Camera on the iPhone**.

The computer finds the iPhone by itself:

- **Over USB** — plug in the cable. The first time, the iPhone asks **“Trust This Computer?”** — tap
  **Trust** and enter your passcode. USB is used automatically whenever the cable is connected
  (latency is only a few milliseconds).
- **Over Wi‑Fi** — both devices on the same network; nothing else to do.

The pill at the top of the Windows app shows **Live · USB** or **Live · Wi‑Fi** when connected.

> **Minimizing the iPhone app stops everything** — camera, microphone and connection. Keep QBT Camera
> open on the iPhone while you stream. To save battery and keep the phone cool, use the
> **black screen** (moon button) instead of locking the phone.

## 5. Use QBT Camera in your apps

In Zoom, Teams, Meet, Discord, OBS or a browser open the video settings and choose the camera
**“QBT Camera”**. It is always in the list — even while the QBT Camera app is closed; in that case it
shows a short “Open QBT Camera on your iPhone and your computer” card.

The camera supports 1080p and 720p formats at 30 fps; the app picks the size, QBT Camera renders
straight into it.

## 6. The Windows app

- **Header** — connection status, camera status (“Camera ready”, “Camera in use”), and the
  **theme button**: Auto (follows Windows) → Light → Dark.
- **Preview** — exactly what your apps receive.
- **Lens switcher** — 0.5×, 1×, telephoto (2×/3×/5× depending on the model), Front.
- **Mirror** button — flips the picture left-right for everyone.
- **Torch** button — the iPhone’s flashlight (back cameras only). If the iPhone is too warm, iOS
  blocks the torch for a while; the app tells you.
- **Tiles** — latency, stream bitrate, frames per second, iPhone heat.

The sidebar has four tabs:

### Camera
- **Exposure** — Automatic, or manual **ISO** and **Shutter** (1/50 and 1/100 avoid flicker under
  50 Hz lights). **Brightness** shifts automatic exposure.
- **White balance** — Automatic, or manual **Temperature** and **Tint**.
- **Focus** — Autofocus, or manual **Distance**.
- **Zoom & torch** — digital zoom within the selected lens, torch brightness.

Moving any manual slider switches that group to manual; turn **Automatic** back on to return.

### Effects
- **Background** — see [Background effects](#7-background-effects).
- **Face** — **Skin smoothing**, **Follow face** (keeps you framed as you move, like Center Stage).
- **Color filter** — eight looks with a **Strength** slider.
- **Color grading (LUT)** — load your own `.cube` LUT.
- **Frame** — **Mirror horizontally**.

### Audio
- **iPhone microphone** — send the iPhone’s sound to the computer, with a level meter.
- **Play audio on this PC** — choose where the sound plays (for monitoring) or **Don’t play**.

### Stream
- **Quality** — 720p / 1080p / 4K and 30 / 60 fps. 4K over Wi‑Fi needs a strong network; USB is best.
- **Orientation** — Auto, Landscape, Flipped, Portrait.
- **iPhone screen** — **Black screen** turns the display off while streaming.
- **Teleprompter by the lens** — show it on the iPhone, start/stop scrolling, speed, **Edit text…**
- **Computer** — **Start with Windows** (QBT Camera starts minimized in the tray).

Closing the window keeps QBT Camera running in the tray (bottom-right corner of the taskbar) so the
camera stays available. To quit, right-click the tray icon → **Exit**.

## 7. Background effects

Choose a mode in **Effects → Background**:

| Mode | What it does |
|---|---|
| **Portrait** | Lens-like blur: the background softens more the farther away it is, highlights turn into round bokeh. **Blur** sets the strength. |
| **Replace** | Cuts you out and puts you on a backdrop: Studio, QBT, Warm, White, Black, **Green** (for keying in OBS), or **your own picture**. |
| **Chroma key** | For a real green (or blue) screen: removes the screen color precisely and clears the green cast from hair and skin. |

### How it works

- **Depth.** On the **1×** camera QBT Camera uses the **LiDAR** scanner, on the **Front** camera the
  **TrueDepth** sensor, together with Apple’s person segmentation. The label under the mode chips
  shows what is used, e.g. *Depth: LiDAR + neural network*. The 0.5× and telephoto cameras have no
  depth sensor and use the silhouette only.
- **Computed on PC / Computed on iPhone.**
  - **Computed on PC** (default, recommended): the iPhone only finds you and measures depth; your
    computer’s graphics card refines the edges at full resolution and composites the result.
    Sharper hair, steadier edges, and the iPhone stays cool.
  - **Computed on iPhone**: everything happens on the phone — useful on a weak computer.
    Your own backdrop picture needs *Computed on PC*.

### Your own background picture

In **Replace** or **Chroma key** choose **Your picture…** and pick a JPEG/PNG. It fills the frame
(cropped to fit). Switching back to a preset backdrop turns it off; the picture is remembered.

### Chroma key tips

- Light the screen evenly and keep about 1 m between you and the screen.
- QBT Camera detects the screen color by itself; after changing the light press
  **Detect screen color again**.
- **Sensitivity** — how much of the screen color disappears. **Edge softness** — softer or crisper edge.
  **Remove green spill** — removes the green tint on hair and skin.

### Best results for Portrait and Replace

- Use the 1× or Front camera (real depth).
- Keep 0.5–2 m between you and the iPhone, with some distance to the wall behind you.
- Avoid strong backlight behind you.

## 8. Color: filters and LUTs

- **Color filter** — Warm, Cool, Cinema, Vivid, Soft, Mono, Noir; **Strength** blends with the original.
- **Color grading (LUT)** — **Load a .cube LUT…** and choose a 3D `.cube` file (sizes up to 256³,
  e.g. from DaVinci Resolve, Premiere or any LUT pack). **LUT strength** blends it in.
  The LUT is applied by the computer to the final picture, in any background mode.
  **Remove LUT** turns it off.

## 9. Audio

The iPhone microphone can be sent to the computer (**Audio → iPhone microphone → Send audio**).
Choose an output under **Play audio on this PC** to hear it. A built-in “QBT Camera” microphone that
appears directly in Zoom/Teams is planned for a future version.

## 10. Stream, teleprompter and iPhone screen

- **Teleprompter** — the text scrolls at the top of the iPhone, right next to the front lens, so you
  read while looking into the camera. Edit the text on the computer (**Edit text… → Send to iPhone**)
  or on the iPhone (pencil button).
- **Black screen** — the iPhone display goes black while the camera keeps streaming. Tap the iPhone
  screen to wake it.

## 11. The iPhone app

- **Top:** status island (connection, latency, quality), quality chip, teleprompter, grid, torch and
  black-screen buttons.
- **Lens row:** 0.5× / 1× / telephoto / Front, mirror and rotate.
- **Panel** (swipe down or tap the handle to fold it): Exposure, Color, Background, Stream tabs —
  the same controls as on the computer, changes show up on both.
- **Double-tap** the picture to fold/unfold the panel, **pinch** to zoom.

## 12. Troubleshooting

| Problem | What to do |
|---|---|
| The computer doesn’t find the iPhone | Keep QBT Camera open on the iPhone. Same Wi‑Fi network? On the iPhone: Settings → QBT Camera → **Local Network** on. Allow QBT Camera in the Windows firewall prompt. Or use a USB cable. |
| USB doesn’t connect | Install **Apple Devices** (Microsoft Store), unlock the iPhone and tap **Trust**. |
| “QBT Camera” missing in an app | Run `Install QBT Camera.cmd` again, then restart that app. |
| Picture stops when I switch apps on the iPhone | Expected: a minimized QBT Camera turns off. Use the black screen instead. |
| Torch won’t turn on | Back cameras only; if the iPhone is warm, iOS blocks the torch for a while. |
| iPhone gets warm | Use **Computed on PC**, the black screen, 1080p 30 fps, and take the case off on long sessions. |
| Choppy 4K over Wi‑Fi | Use USB or 1080p. |

## 13. Uninstall

Run **`Uninstall QBT Camera.cmd`** from the package. It removes the app, the virtual camera and the
Start menu shortcut. Settings in `%LOCALAPPDATA%\QBTCamera` can be deleted by hand.

# chiaki4xbox

PlayStation Remote Play for **Xbox Series X|S (Dev Mode)**, built on [chiaki-ng](https://github.com/streetpea/chiaki-ng).
Stream your PS5 to your Xbox with the Xbox controller.

## Features
- 1080p / 60 fps PS5 streaming, HEVC or H.264, hardware decoding (Media Foundation + D3D11)
- Low-latency swap chain and XAudio2 audio
- chiaki-ng style UI: console list, multiple PSN profiles, on-screen keyboard, console IP setting, help screen
- In-stream settings menu: detail restore, deband, sharpening, color boost, frame generation (30 → 60 fps), FPS counter, resolution (720p/1080p), bitrate (10–50 Mbps), codec
- Rumble

## Controls while streaming
| Button | Action |
|---|---|
| Hold **Menu** + **D-pad Up** | Open / close the settings menu |
| Hold **Menu** + **D-pad Down** | PS button |
| Tap **Menu** | Options |
| **View** | Touchpad click |

## Setup
1. Put your Xbox in **Dev Mode** and enable the **Device Portal**.
2. Install the `.msix` (from Releases or your own build) through Device Portal → *Add*.
3. Open the app, press **+ Add Profile**, type a name and the **PSN account ID** (base64) of the account signed in on the PS5.
4. Set your PS5's IP address with the **IP** button.
5. On the PS5: *Settings → System → Remote Play → Link Device*, then enter the 8-digit PIN in the app.

Tip: you can find your base64 account ID with any chiaki-ng "PSN account ID" helper.

## Building (Windows)
Requirements: Visual Studio 2026 with the *Desktop C++* and *UWP* workloads, CMake, Git.

```
cd xbox
powershell -ExecutionPolicy Bypass -File make-cert.ps1   # one time
build.bat
```
The package lands in `xbox\AppPackages\`. The first build downloads OpenSSL and Opus through vcpkg and takes a while.

The four files in `lib/src` (`ctrl.c`, `regist.c`, `session.c`, `remote/rudp.c`) carry small MSVC patches (no VLAs) so chiaki-ng's core compiles for UWP.

## Notes
- 1440p and 4K are not offered: the PS5 refuses them in this setup.
- Not affiliated with Sony or Microsoft. Licensed under the AGPL-3.0 like chiaki-ng.

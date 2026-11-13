# wxr-vid-cap
Video capture locally or over WiFi to a recording device for better representation of WinlatorXR experiences without the Quest native recording overhead

## Building

Requires Visual Studio 2022 (Community is fine) with the **Desktop development with C++**
workload and a Windows 10/11 SDK.

- Open `wxr-vid-cap.sln` and build, or:
- `msbuild wxr-vid-cap.sln /p:Configuration=Release /p:Platform=x64`

Output lands in `build\x64\<Configuration>\wxr-vid-cap.exe`. Release uses the static CRT, so the
executable depends only on system DLLs and can be copied into a Winlator container without
shipping a VC++ redistributable.

## Running

The app is tray-only: launch it and look for the red record dot in the notification area.
Double-click it (or right-click → `SHOW`) to open the **WXR Capture Tool** window, which has the
same actions as the tray menu:

- `SHOW` — opens the WXR Capture Tool window.
- `RECORD LOCAL` — chooses a capture source, asks for the MKV path, and starts recording.
- `CAST LOCAL` — asks for the receiver IP (prefilled with the last one), then the capture source,
  and starts the live MPEG-TS/UDP stream.
- `STOP RECORDING` / `STOP CASTING` — stops the active session.
- `SELECT CAST SOURCE...` — while casting, opens the capture source screen to switch the live cast
  to another running application, the top-most window, or a newly launched application.
- `EXIT` — stops any session and exits.

If the selected window closes during a session, recording tries to re-hook the top-most window and
stops with an error if none is available. Casting keeps running: it sends blank video and silent
audio, shows `waiting for a source` in the WXR Capture Tool window, and retries the top-most
window periodically. Only starting a record/cast and fully stopping it show a balloon; source
changes and retries are silent.

Every `RECORD LOCAL` or `CAST LOCAL` opens the capture source screen:

1. `Capture the top-most window` (default) — resolves to the current top-most hookable application.
2. `Capture a running application` — pick from the list of windows.
3. `Start and inject capture into an application` — recommended for D3D11/12 and Vulkan, where the
   hook must be present before the app creates its device. Use `Browse...` to choose the EXE.

Use `Refresh` to re-scan the running-applications list after starting or closing a game.
`OK` proceeds; `Cancel` cancels the record or cast.

`capture-blacklist.txt` beside the app lists executables that must never be captured or launched
with the hook. It ships with `taskmgr.exe`, `explorer.exe`, `notepad.exe`, and Winlator's
`wfm.exe`; edit it and restart the app to change the list. Blacklisted apps are also removed from
the running-applications list.

Capture, encoding, and audio work for both local recording and LAN casting. `RECORD LOCAL` opens a
Save dialog prefilled with `D:\WXRVIDCAP\capture_YYYY-MM-DD_hh_mm_ss.mkv` (the directory is created
when missing) and writes an H.264 + Opus MKV at 1280×720, 24 fps, 4 Mbps video, 96 kbps audio. With a
target selected it captures inside the process through the graphics API — DirectDraw
`Blt`/`BltFast`/`Flip`, D3D9 `EndScene`, D3D10/D3D11 `Present`, D3D12 `Present` with a launch-time
device capture, OpenGL `SwapBuffers`, and Vulkan `vkQueuePresentKHR`. That is the only way to see
DXVK surfaces. D3D12 and Vulkan targets should use the launch option; other targets can be injected
while running. There is no GDI fallback: if the hook cannot start, recording fails with the hook
error. While recording, the tray tooltip shows elapsed time and the recording stops automatically at
15 minutes.

`CAST LOCAL` runs the same capture and encode path but muxes H.264 and Opus into MPEG-TS and sends
it over UDP to the receiver IP. It writes no file and does not wait for a receiver
acknowledgement: VLC can open `udp://@:<port>` before or after the cast starts. The IP dialog has a
touch keypad (`7 8 9 / 4 5 6 / 1 2 3 / 0 . BSP`); a blank address starts at `192.168.0.`, and the
last address used is prefilled next time (port 1234 by default). Use a VLC network cache of
200–300 ms; the caster also buffers 8 frames (`[cast] jitter_frames`, 0 disables) before paced
sending, which smooths playback at the cost of about a third of a second of extra latency. If no
source can be hooked, the cast continues with black video and silent audio while a background probe
keeps retrying. An application chosen in the source screen stays selected: while its process is
open, the caster keeps retrying that same app instead of switching to another window, and the
default top-most pick is pinned once it is hooked. Use `SELECT CAST SOURCE...` to choose a
different app. A launch-through-hook cast waits 30 s for the first frame, then 30 s more after
asking the resident hook to re-scan its loaded modules.

Start and stop play short two-beep cues (low→medium on start, high→medium on stop), and a third low
"dun dun" cue plays when a manual record/cast start or launch cannot hook. Run
`wxr-vid-cap.exe --test-sounds` to hear all three and write `record_start.wav`, `record_stop.wav`,
and `record_hook_failure.wav` beside the app.
`wxr-vid-cap.exe --hook-debug` runs the old 3-second hook diagnostic and writes `hook_debug.bmp`.

The hook ships as `wxr_hook_dll.dll` (x64) and `wxr_hook_dll32.dll` (x86), injected by the app or
by `wxr_inject32.exe` for 32-bit targets. Run `build.ps1` to build all of them — the solution is
x64-only, so the 32-bit targets are built separately.

Only x64 is configured for the app; Winlator runs x64 binaries under Box64.

## Capture test apps

`tools/testapps` builds one small windowed app per graphics API for testing capture in a container:

| App | API |
| --- | --- |
| `cube_ddraw.exe` | DirectDraw 2D (windowed `Blt`, `--fullscreen` for `Flip`) |
| `cube_d3d7.exe` | Direct3D 7 (DirectDraw 7 + D3D7 immediate mode) |
| `cube_d3d8.exe` | Direct3D 8 |
| `cube_d3d9.exe` | Direct3D 9 |
| `cube_d3d10.exe` | Direct3D 10 |
| `cube_d3d11.exe` | Direct3D 11 |
| `cube_d3d12.exe` | Direct3D 12 |
| `cube_gl.exe` | OpenGL (compatibility profile, immediate mode) |
| `cube_vk.exe` | Vulkan |

Each opens an 800×450 window with a spinning cube whose six faces get random colors, plays a
looping bytebeat tone, and shows the live frame rate in the title bar. `cube_ddraw` instead shows a
rotating 2D rectangle with a `DDRAW` label, using only DirectDraw. Pick one in the capture source
list, then use `RECORD LOCAL` or `CAST LOCAL`.

`build.ps1` builds all of them into `build\testapps\Release`. Each app writes `testapp.log` beside
itself with the API setup result and audio status, which is useful when a container lacks a runtime.

Notes:

- The capture hook handles DirectDraw (`Blt`, `BltFast`, `Flip`, legacy and 7 interfaces), D3D7,
  D3D8, D3D9, D3D10, D3D11, D3D12, OpenGL, and Vulkan. D3D12 and Vulkan need launch-time injection
  (`wxr_launch.exe` / `wxr_launch32.exe`), which the capture source screen's `Start and inject
  capture into an application` option uses. The container's d3d8 is 32-bit (the D3D8 games are
  32-bit), so `cube_d3d8.exe` is built for Win32 and the x64 host injects the 32-bit hook.
- Every app accepts `--fullscreen`, which creates a borderless window at the desktop resolution.
  `cube_runner` runs the whole set windowed and fullscreen, in both 64-bit and 32-bit builds
  (`<app>.exe` and `<app>32.exe`). Vulkan is 64-bit only here.
- D3D8 and D3D7 need the container's `d3d8.dll` / D3D7 runtime; Windows 11 itself no longer ships
  them, so those two are container-only. `cube_d3d7.exe` falls back to a painter's-algorithm sort
  when the driver rejects a Z-buffer surface.
- The Vulkan app links `vulkan-1.dll`, and D3D12 needs vkd3d, so those two depend on what the
  container provides.
- `tools/testapps/gen_shaders.ps1` regenerates the embedded shader blobs after editing
  `shaders.hlsl`, `cube.vert`, or `cube.frag`. The generated header is committed, so normal builds
  do not need fxc or glslangValidator.

### Automated capture run

`cube_runner.exe` runs every test app four ways — windowed/fullscreen × 64-bit/32-bit — starting
each app, waiting for its frame-rate title, recording six seconds through the normal recorder
(graphics hook only; a missing hook is logged and produces no MKV), stopping it, and keeping every
MKV. Fullscreen uses a borderless window at the desktop resolution, matching how games present.

```
cube_runner.exe
```

It writes `cube_tests\<timestamp>\cube_tests.log` beside itself with a `## windowed64`,
`## windowed32`, `## fullscreen64`, and `## fullscreen32` section. Each line reports the backend,
injection method, source resolution, `visible`/`black`, frame count, and video file
(`d3d9_windowed64.mkv`, `d3d9_windowed32.mkv`, ...). Videos are never deleted. `build.ps1` copies
`record_5s.exe`, `openh264.dll`, the launchers, the hook DLLs, and both app architectures next to
the runner.

The 32-bit passes use `<app>32.exe` and inject through `wxr_inject32.exe` (or `wxr_launch32.exe`
for D3D12). Vulkan is 64-bit only because there is no 32-bit Vulkan import library here.

The runner kills any leftover `cube_*.exe` before starting, and treats a `black` result as a
warning rather than a failure, so a run reports `N ok (M black), K failed`.

## Tests

`wxr-vid-cap-tests` is a console project in the same solution. It checks the MPEG-TS muxer, the
UDP sender, and the Matroska writer without needing media files:

```
msbuild wxr-vid-cap.sln /p:Configuration=Release /p:Platform=x64
build\tests\x64\Release\wxr-vid-cap-tests.exe
```

Exit code 0 means every check passed.

### Local capture probe

Captures the desktop with the in-tree capture path and writes an uncompressed AVI:

```
build\tools\x64\Release\record_test.exe --seconds 2 --fps 15 --width 1280 --height 720 --out build\capture\preview.avi
```

Capture follows the top-most window and writes uncompressed frames, which makes the file large;
this is a capture and lifecycle probe, not the final recorder. These standalone probes still use
the GDI `ScreenCapture` path and are useful only on a Windows desktop; the tray app uses the
graphics hook and has no GDI fallback.

### Encoded recording probe

`encode_test` captures, converts to I420, encodes H.264 with the vendored OpenH264 DLL, and writes
Matroska beside the EXE:

```
build\tools\x64\Release\encode_test.exe
```

Defaults: 3 s, 15 fps, 1280×720, 4000 kbps. `openh264.dll` must sit next to the
EXE; the build copies it there from `third_party\openh264\bin`.

### Five-second HMD recorder (Windows-only GDI probe)

`record_5s.exe` takes no arguments: double-click it and it records 5 seconds to a timestamped MKV
beside itself, then shows the path in a message box. A `<name>.mkv.log` with per-stage timings and
a `windows:` inventory (`title - EXE.exe  (full\path\EXE.exe)` per visible window, `no title` when
the caption is empty) is written next to the recording. The log also records `window:` (what was
actually captured), `content: visible|black`, and `method: printwindow|window-dc|screen`.
This standalone probe still uses GDI and is useful only on a Windows desktop; the tray app uses
the graphics hook and has no GDI fallback.

### Cast smoke test

`cast_test_source` feeds an H.264 elementary stream (and optional AAC ADTS audio) through the
muxer and UDP sender. Point it at a stream you have the rights to use; no fixture generator is
shipped. VLC is a client viewer only and is never used to capture. The tray app's own cast uses
the same muxer with Opus audio, including the PMT registration/audio descriptors and the OpusTS
syncword framing that VLC 3.0 requires.

```
build\tools\x64\Release\cast_test_source.exe --file <stream.h264> --audio <stream.aac> --target 127.0.0.1 --port 1234 --seconds 5
```

`--audio` is optional; without it the stream is video-only.

Project goals and task tracking live in `Docs/OpenCode/DeepSeek41Flash/`.

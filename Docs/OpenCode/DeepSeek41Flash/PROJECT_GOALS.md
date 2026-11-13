# Project Goals

`wxr-vid-cap` is a C++ capture tool for Winlator (WinlatorXR and other forks). It records
screen and audio from games running inside the Winlator container, so gameplay footage can be
produced without paying the Quest-native recording overhead.

The four goals below define the product. Each has the reason it exists, the constraints that
shape the implementation, and what "done" looks like.

---

## 1. Minimal-overhead capture, agnostic to the Winlator fork

**Goal:** Capture screen and audio with as little CPU/GPU cost as possible, and work on most any
fork of the Winlator APK rather than being tied to one build.

**Why:** The host is an Android device running Wine under Box64/Box86 emulation on top of a
Quest headset or phone. Every frame of overhead competes directly with the game, and forks
diverge in container layout, graphics driver (Turnip, Vortek, VirGL), Wine build, and launcher
paths.

**Constraints:**

- Assume x86/x86_64 Windows binaries running under Wine + Box64 — no reliance on native
  Android/JNI capture APIs, and no assumption that a fork ships a specific helper service.
- Must not hard-code container paths, driver names, or fork-specific configuration.
- The capture source must survive DXVK/VKD3D output paths and whatever swapchain presentation
  the container uses; emulated CPU time for capture must stay small relative to the game.
- Prefer GPU-side encode where a usable encoder is present, with a CPU fallback that still
  stays within budget.
- Capture must see DXVK/VKD3D-presented content. External GDI/PrintWindow cannot: the window target
  is correct, but the presented surface is not in the desktop framebuffer. The reliable path is an
  injected capture DLL that hooks the graphics API inside the target process.
- Hook targets, in order of implementation: D3D11 `IDXGISwapChain::Present` (DXVK), D3D9
  `EndScene`/`Present` (DXVK/WineD3D), OpenGL `wglSwapBuffers` (VirGL), Vulkan
  `vkQueuePresentKHR` (implicit layer, last).
- The injected DLL copies frames into a named shared-memory ring; the host tray app converts,
  encodes, records, and casts. The DLL does not encode.
- Injection is x64 `CreateRemoteThread` + `LoadLibrary`; 32-bit targets will need a 32-bit DLL.
- **Chain-last hooking:** mods commonly hook `Present`/`EndScene`/`wglSwapBuffers` already. Read the
  *current* vtable/function pointer and hook that, not the stock original. Our detour calls the
  previous pointer, so existing mod hooks run and we capture their final output. Never replace,
  disable, or remove another hook; restore only ours on unload. If the entry changes because a mod
  loaded after us, re-hook the new entry so we remain last in the chain.
- Test chain-last behavior with a synthetic pre-hook that alters the frame: the captured image must
  show the alteration, and the app must keep rendering normally.
- GDI/PrintWindow capture stays as a fallback for non-GPU windows and as a diagnostic path.
- Risks: anti-cheat, per-fork injection restrictions, and stability. Test on-device before
  shipping; never inject into a process the user has not selected.
- Encoding may use a statically linked, permissively licensed open-source encoder (allowed
  2026-11-10), as long as it ships inside our binary and needs no external app or runtime
  install. GPL code is excluded by this project's MIT license.
- On-device GDI (2026-11-11): ~10 ms capture, ~8.6 ms convert, ~6.1 ms encode, ~1.3 ms audio,
  ~26 ms/frame at 720p24. One 245 ms outlier was traced to HALFTONE scaling; COLORONCOLOR is used
  now.
- Media Foundation probe in WinlatorXR (2026-11-10): `mfplat.dll` present, `MFStartup` returns
  `S_OK`, but **zero H.264 encoders and zero AAC encoders** are registered; only one H.264
  decoder. MF cannot be used for local recording in this fork.
- Encoder: **OpenH264 2.4.1** (BSD-2) is vendored under `third_party/openh264` and loaded from
  `openh264.dll` beside the EXE. On-device (2026-11-11): 1280x720 at 15 fps, 45 frames, 77 KB for
  3 s. Per-frame cost under Box64: capture 9.23 ms, convert 9.05 ms, encode 7.41 ms (25.7 ms
  total). Comfortable at 15 fps; 30 fps leaves little headroom for the game.
- Audio: WASAPI loopback works on-device (confirmed with Netstorm's wind sound). The container mixes
  at 44.1 kHz, so a linear resampler converts to 48 kHz before Opus. Opus 1.5.2 (BSD) is statically
  linked and muxed as `A_OPUS`.
- GDI scaling switched from HALFTONE to COLORONCOLOR after a 245 ms/frame on-device run; the
  recorder now writes a `.mkv.log` beside each file with per-stage averages so capture cost can be
  diagnosed from the container.

**Done when:** The tool installs into a stock Winlator container, captures screen and audio
from a running game, and sustains recording without a measurable drop in game frame rate on the
reference device — on at least two different forks.

---

## 2. Short clips of 10–15 minutes or less

**Goal:** First-class support for short, bounded recordings, capped at 10–15 minutes.

**Why:** Long continuous recordings on this hardware degrade over time — memory growth in the
encoder, growing buffers, thermal throttling, and emulation jitter all compound. Short clips
keep quality predictable and fit the way gameplay highlights are actually shared.

This is the only mode that writes a file. Casting (goal 3) records nothing, so there is no
second writer to coordinate with.

**Constraints:**

- Hard limit on duration (configurable within the 10–15 minute window), with automatic stop and
  clean finalize rather than an abrupt truncation.
- Bounded memory: no unbounded queue between capture and encode, and no "keep everything in RAM
  then write at the end" design.
- Output must be a playable file immediately after the stop, even if the process is closed via
  the tray.
- Default output location is `D:\` inside Winlator, which is the Android host's `Downloads`
  directory. The Save dialog opens there; the user can change it.
- Warn or block when free space or encoder pressure cannot sustain the configured clip length.

**Done when:** A 15-minute clip records end to end at stable frame rate and lands as a valid file
that plays in a standard player, with memory and encode latency flat across the clip.

---

## 3. Live LAN cast to a VLC receiver

**Goal:** Cast live video and audio over the LAN to a viewer running VLC. The app records nothing
in this mode; if the viewer wants a file, they record it themselves.

**Why:** Recording the long session on the headset is what causes the degradation in goal 2.
Casting removes storage and mux-to-file work from the container entirely — the capture side only
encodes and sends packets. VLC speaks UDP natively, so no relay, no web page, and no browser
transport problem exists.

**Capture target (shared with recording):** the tray SHOW window lets the user capture the
top-most window (default, `wfm.exe` excluded) or a specific EXE chosen from the running processes.
The same saved target is used by START RECORD and START CAST.

**What the viewer does:**

1. Opens `udp://@:1234` in VLC (or any player that accepts MPEG-TS over UDP).
2. Watches. If they want a file, they record it themselves — VLC's Record button, OBS, FRAPS, a
   phone screen recorder. The project ships no receiver, no page, and no recording tool.

**Transport:**

- MPEG-TS over UDP, unicast to one viewer IP. Not multicast: one viewer at a time, no network
  flooding, no Wi-Fi multicast rate collapse.
- H.264 video plus Opus audio in a single program, so VLC needs no SDP or extra configuration.
- Implemented in-tree: PAT/PMT every 0.5 s, PCR every 100 ms on a dedicated PID, video PES with
  PTS only, audio as Opus packets with the OpusTS syncword header. The PMT carries the Opus
  registration descriptor and Opus audio descriptor so VLC selects the audio track. The encoder
  must not emit B-frames, because no separate DTS is carried.
- Structural tests cover PAT/PMT CRCs, PES framing and PTS round-trip, PCR cadence, continuity
  counters, OpusTS framing, and UDP datagram batching.
- Validated on Windows loopback (2026-11-10): a 5 s synthetic A/V stream muxed by this code and
  sent over UDP was received, demuxed, and decoded by VLC. Video came out as a 4.57 s MP4; audio
  came out as 4.74 s of PCM and the MP4 carried both `avc1` and `mp4a` tracks. The gaps are VLC
  locking onto the stream at join, not packet corruption.
- Validated with the real `Caster` (2026-11-12): a hooked D3D9 test app was captured and sent over
  loopback UDP; VLC 3.0.24 recognized the Opus stream and decoded it as two-channel audio alongside
  H.264 video. On-device LAN validation is still open.
- VLC's network cache should be set low (200–300 ms) for usable latency; document that rather
  than fight it.

**Mode contract:**

| Mode | Output | Container cost |
| --- | --- | --- |
| START RECORD | MKV file on local storage (goal 2) | Encode + local write |
| START CAST | None. Live packets only; viewer records if they choose. | Encode + UDP send |

The two modes are mutually exclusive. Casting never touches disk — no temp file, no second mux,
not even a short buffer written out "just in case".

**Constraints:**

- No file handle is opened in cast mode. If the user wants a file, that is START RECORD.
- The app shows the exact URL to open (`udp://@:<port>`) and the target address it is sending to.
- A missing or unreachable target fails loudly and returns to idle; it must not silently stream
  into the void.
- The target address is entered in the UI (tray `Set Receiver IP...`) and saved between sessions.
  `START CAST` prompts for it only when nothing is saved; it never guesses.
- Latency target is under one second end to end with VLC's cache lowered.
- Packetization must not allocate per frame in a way that grows over time; the sender runs for
  the whole session.

**Done when:** VLC on a phone or PC plays the live stream at a stable frame rate for a full
15+ minute session, the container writes nothing to disk, and game frame rate is unaffected
compared to the baseline from Phase 1.

---

## 4. Background operation with a tray icon

**Goal:** The app runs in the background and exposes a tray icon the user can open.

**Why:** Capture is a utility, not a windowed app to alt-tab to. In a container the user is
already inside a game; a resident tray presence keeps start/stop and status one click away
without stealing focus or screen space.

**Tray flows (updated 2026-11-12):**

The tray icon and the **WXR Capture Tool** window expose the same actions. Only one mode can be
active at a time.

*Idle menu:* `SHOW` · `RECORD LOCAL` · `CAST LOCAL` · `EXIT`
*Recording menu:* `SHOW` · `STOP RECORDING` · `EXIT`
*Casting menu:* `SHOW` · `STOP CASTING` · `SELECT CAST SOURCE...` · `EXIT`

**SHOW**

Opens the WXR Capture Tool window, which carries the same action buttons. It stays open while a
session runs, shows the current capture source, and updates its button and status line when the
mode changes.

**RECORD LOCAL**

1. Opens the capture source screen.
2. On **OK**, opens the standard Windows Save dialog in `D:\` — the Android `Downloads` folder as
   mounted inside Winlator — with a suggested name of `recording_YYYY-MM-DD_hhmmss`, where the
   timestamp is captured when the Save dialog opens.
3. On **OK**, recording starts immediately. On **Cancel**, nothing starts and the app stays idle.
4. If the source was `Start and inject capture into an application`, the selected EXE is launched
   with the capture hook first, then the recorder waits up to 30 seconds for its first frame.
5. Records to a local MKV until `STOP RECORDING` or exit. No network activity in this mode.

**CAST LOCAL**

1. Opens the receiver IP dialog, prefilled with the last IPv4 address entered. Cancel stops the
   flow before anything starts.
2. On **OK**, opens the capture source screen.
3. On **OK**, starts streaming immediately to that address. If the source was the launch option,
   the EXE is started with the hook first.
4. Shows the exact URL the viewer opens (`udp://@:<port>`) and the address being sent to.
5. Records nothing, locally or remotely. The viewer brings their own recording tool if they want
   a file. Runs until `STOP CASTING` or exit.

**Capture source screen**

1. `Capture the top-most window` (default) — GDI fallback path.
2. `Capture a running application` — choose from the list of visible windows.
3. `Start and inject capture into an application` — **Recommended for D3D11/12 and Vulkan**;
   `Browse...` selects the EXE. The app is launched with `wxr_launch.exe` or `wxr_launch32.exe`
   depending on its bitness.

`OK` proceeds; `Cancel` cancels the record or cast.

**Source changes**

- If the selected window closes during recording, capture automatically switches to the top-most
  window. No source dialog is shown in recording mode.
- If it closes during casting, the same automatic switch happens; the cast keeps running and the
  WXR Capture Tool source line updates.
- While casting, `SELECT CAST SOURCE...` opens the capture source screen to switch the live cast
  manually. Cancel leaves the current cast untouched.
- If the automatic re-hook succeeds but produces no frames (for example, a non-graphics app), the
  session falls back to GDI top-most capture after about two seconds.
- The frame bridge is named after the target process, so a hook DLL left in an older application
  cannot feed frames into a later session.

**While a mode is active**

- The menu collapses to `STOP RECORDING` or `STOP CASTING`; the other mode cannot be started.
- While casting, `SELECT CAST SOURCE...` remains available.
- `SHOW` remains available and shows the same actions, elapsed time, destination, and current source.
- `STOP` ends the session: a local recording is finalized to a playable file; a cast session just
  stops sending.
- `EXIT` (and closing the app any other way) performs the same stop first. A recording must never
  be left half-written without being finalized or clearly marked as damaged.

**Constraints:**

- Launch to background (optionally at container start), no console window, no focus grab.
- Near-zero idle cost when not recording, and no dropped input while recording.
- Exactly one active session, enforced by the UI and by the state machine underneath it.
- Tray behavior must not depend on a fork-specific desktop shell being present.
- If the Save dialog cannot open in a given fork, fail with a visible message rather than
  silently recording to an unknown path.

**Background budget (measured 2026-11-10, Windows x64 Release):**

| Metric | Idle value |
| --- | --- |
| CPU | 0 ms over 10 s (below the 15.6 ms timer granularity) |
| Private bytes | 1.4 MB |
| Working set | 7.0 MB (mostly shared system DLL pages) |
| Handles | 128 |
| Threads | 4 |
| Loaded modules | 25 |

Rules that keep it there:

- Event-driven only: no timers and no polling. Work happens on tray commands, window messages, and
  OS notifications.
- Optional DLLs are delay-loaded: `comdlg32` (Save dialog) and `ws2_32` (casting) are not mapped
  until first use.
- Single instance through a named mutex. A second launch posts a "show menu" message to the running
  copy and exits, so there is never a second idle process or tray icon.
- Nothing is created at idle beyond the tray icon: no GDI objects, no worker threads, no timers.
- Re-measure after each feature. Idle CPU above measurement noise, or private bytes above ~2 MB,
  needs a justification.

Private bytes is the number to watch; working set includes pages shared with every other process.

**Open details (small, decide during implementation):**

- Container/extension for the saved file: **MKV**, decided 2026-11-10 for crash tolerance. The
  suggested name carries no extension; the app appends `.mkv`.
- A stop that produces zero usable frames: discard the empty file and say so, rather than
  leaving a 0-byte recording behind.
- Name collision on the auto-suggested file: let the Save dialog's overwrite prompt handle it.
- Receiver address: entered in the UI and persisted (decided 2026-11-10), so it is set once and
  reused. Stored as `receiver_ip` plus `port` (default 1234) in `settings.ini`.
- `D:\` is the host's `Downloads` directory in Winlator, but that mapping is per-fork. The Save
  dialog is the fallback when the drive is missing or read-only.

**Done when:** The tool sits in the tray through a full game session, both modes start and stop
from the tray alone, the two modes cannot run together, and exiting from the tray always leaves
the last recording in a usable state.

---

## Toolchain

- **Visual Studio 2022 Community** solution (`wxr-vid-cap.sln`), MSBuild, v143 toolset.
- **C++20**, `/W4`, Unicode, Windows-subsystem binary (no console window).
- **x64 only** for now — Winlator runs x64 binaries under Box64. An x86 configuration can be
  added later if a fork/device needs it.
- **Static CRT** (`/MT`) so the Release executable drops into a container with no VC++
  redistributable; verified dependencies are `KERNEL32` and `USER32` only.
- Dependencies are added deliberately, one at a time, with licensing noted — no blanket package
  manager by default.

## Non-goals (for now)

- General-purpose editing, trimming, or uploading of footage.
- Shipping a receiver: no relay app, no viewer page, no in-browser recorder. The viewer brings
  VLC (or another player) and their own recording tool.
- Bundling, driving, or supporting a particular capture tool (VLC Record, FRAPS, Hypercam, OBS).
- Streaming to public platforms (Twitch/YouTube) as a product feature — casting is unicast to one
  LAN viewer, not a broadcast CDN.
- Replacing OBS on the desktop side.

## Rejected approaches

Kept here so they don't get re-proposed without new information.

- **Browser page as the receiver, with a relay app or a Quest-hosted page.** A browser cannot
  receive UDP, so every variant needs a relay process, an HTTP/WebSocket pipeline, or a
  Quest-side server, and none of them can record without extra software. VLC does the same job
  with far less code. Rejected 2026-11-10.
- **Flash + Ruffle as the viewer/receiver.** Flash exposed only TCP sockets (`Socket`, `XMLSocket`,
  which also need a port-843 policy file); raw UDP never existed outside RTMFP and Adobe Cirrus,
  both dead. Ruffle's web build reaches TCP only through a WebSocket-to-TCP proxy, so it still
  requires a relay process, and a page still cannot bind a UDP port. Dead platform, extra WASM
  runtime, same wall. Rejected 2026-11-10.
- **Page hosted on the public webserver, capture app sends UDP to the viewer's IP.** A page cannot
  receive UDP, and NAT blocks direct delivery to a viewer off-LAN. Rejected 2026-11-10.
- **Direct UDP to the viewer device with no intermediary.** Requires a native receiver app on that
  device. Superseded by VLC, which is that app but one users already have.
- **Media Foundation encoding on the tested Winlator fork.** `mfplat.dll` and `MFStartup` work,
  but the fork registers no H.264 or AAC encoder MFTs, so there is nothing to encode with. Rejected
  2026-11-10; MF may still be worth re-checking on other forks.
- **DXGI desktop duplication.** Works on the dev PC (~2.8 ms/frame) but the target Winlator fork's
  DXVK returns an error from `DuplicateOutput`, so the backend was removed. Rejected 2026-11-11.
- **Windows.Graphics.Capture (WGC).** Wine has no complete WinRT capture implementation capable of
  grabbing DXVK/Vulkan swapchains in Winlator. Rejected 2026-11-11.
- **External GDI/PrintWindow for GPU-rendered content.** The target window can be selected and the
  title reported correctly, but the presented surface is not in the desktop framebuffer, so the
  result is black. Kept only as a fallback for non-GPU windows. Rejected as the primary path
  2026-11-11.
- **VLC screen capture as a fixture or capture source.** VLC is a viewer on client devices only.
  Capturing the user's desktop through it is not acceptable, and the capture pipeline must be
  self-made against raw Windows APIs. Fixtures generated that way were deleted. Rejected
  2026-11-10.

## Open questions

| Question | Why it matters |
| --- | --- |
| Which capture path is reliably available inside the container (presentation hook, D3D/GL interop, framebuffer readback)? | Determines whether goal 1's overhead target is reachable at all. |
| Which encoder is present across forks: hardware (AMF/VAAPI-class) via the driver, or a bundled software encoder? | Drives dependency size and the encode budget. |
| Which encoder can we actually use inside Winlator: OpenH264 (BSD), minih264 (public domain), or libjpeg-turbo MJPEG (BSD)? | MF has no encoders in the tested fork; OpenH264 is vendored, but its Box64 performance is unmeasured. |
| Audio: Opus (BSD, royalty-free, `A_OPUS` in MKV) or raw PCM? | MF has no AAC encoder; Opus is the lightweight choice, PCM is trivial but enormous. |
| Which hooking library: MinHook (BSD-2), Microsoft Detours (MIT), or Kiero (MIT) for vtable setup? | Determines code size, maintenance, and whether Vulkan needs a separate implicit layer. |
| Does the target fork allow `CreateRemoteThread` + `LoadLibrary` injection, and does Box64 run the hook DLL reliably? | The whole hook architecture depends on this; it must be tested before building the full bridge. |
| Which renderer does MPC-HC use in the container (D3D9 EVR vs D3D11 MPC Video Renderer)? | Decides whether the D3D9 or D3D11 hook is needed first for the test case. |
| MPEG-TS or RTP/H.264 for the cast transport? | TS needs no SDP and VLC auto-detects it; RTP is slightly leaner but needs session description. TS is the default assumption. |
| Can the container open a listening socket at all, if we ever want a handoff page for address discovery? | Only matters for the optional zero-config path; sending UDP is the low-risk part. |

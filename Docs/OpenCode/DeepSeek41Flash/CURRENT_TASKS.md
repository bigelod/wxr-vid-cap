# Current Tasks

Work in progress, in priority order. The project is at the scaffolding stage: the repository has
a README and `.gitignore` and no source yet, so the first block is about choosing foundations
before writing capture code.

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done (move to `COMPLETED_TASKS.md`)

## Phase 0 — Foundations

- [x] **Build system: Visual Studio 2022 Community solution.** `wxr-vid-cap.sln` + MSBuild,
  v143 toolset, C++20, x64, static CRT — see `COMPLETED_TASKS.md`.
- [ ] **Dependency policy.** With CMake and vcpkg out of the build, decide per dependency whether
  it is vendored or fetched. The cast path needs an MPEG-TS packetizer and a UDP socket; prefer
  writing the TS muxer in-tree over pulling in FFmpeg. Record licensing before adding anything.
- [ ] **Verify toolchain on device.** Copy `build\x64\Release\wxr-vid-cap.exe` into a Winlator
  container and run it; it should show the placeholder message box. No capture code until that
  works.
- [ ] **Code conventions.** Keep the tree `/W4` clean, C++20, RAII wrappers for Win32 handles,
  no console window in the shipping subsystem. Add `include/` and `tests/` only when they have
  something in them.

## Phase 1 — Capture feasibility spike

- [x] **GDI capture baseline.** `src/screen_capture.*` captures the desktop with BitBlt into a
  top-down DIB. Measured 26.6 ms/frame at 1920x1080 on the dev PC, before scaling or encoding.
- [x] **Target-size capture.** StretchBlt straight into the recording size works but costs
  36.5 ms/frame for 1280x720 with HALFTONE, so it is not a performance shortcut.
- [x] **GDI is the only backend.** DXGI duplication was removed after the target DXVK rejected
  `DuplicateOutput`. GDI follows the top-most window and letterboxes to 720p.
- [ ] **Validate window-follow on-device.** The existing logs prove the pipeline (121 frames/5 s,
  ~26 ms/frame), but they predate the crop. Re-run `record_5s.exe` with the app windowed and send
  the new `.mkv.log`.
- [ ] **Tune for Quest 3.** On-device GDI was ~10 ms capture at full desktop; confirm the crop
  does not add significant cost. If needed, reduce to 540p or 20 fps.
- [x] **Survey encoders.** `tools/mf_probe.exe` in WinlatorXR: MF present, `MFStartup` S_OK, but
  **zero H.264/AAC encoders**. MF is not usable for encoding in this fork.
- [x] **Vendor OpenH264 for video.** `third_party/openh264` with BSD license; loaded from
  `openh264.dll` beside the EXE. Local encode test passes at 720p/15 with 3.75 ms/frame.
- [x] **Test OpenH264 on-device.** `encode_test.exe` in Winlator: 45 frames at 720p15, 25.7 ms per
  frame total (capture 9.23, convert 9.05, encode 7.41). Works; 30 fps is tight but 15 fps is
  comfortable.
- [x] **Test WASAPI loopback on-device.** `audio_probe.exe` in Winlator captured the game audio
  (Netstorm wind), so loopback works under this Wine build.
- [x] **Vendor Opus for audio.** 1.5.2 (BSD), static x64 `/MT`, `A_OPUS` track in MKV.
- [ ] **Test A/V encode on-device.** Run `encode_test.exe` with game audio and report
  `encode_log.txt`; compare total ms/frame against the 15/24/30 fps budgets.
- [x] **Wire the pipeline into the tray recorder.** `Recorder` now runs capture, OpenH264, Opus,
  and `MkvWriter` on a worker thread at 720p24, 4 Mbps, 96 kbps audio. It writes a `.mkv.log` with
  per-stage timings beside every recording.
- [x] **Top-most window capture.** GDI now re-checks the foreground/Z-order every frame, captures
  the window's client area, and letterboxes to 1280x720 so switching apps mid-recording works.
  Menus, tooltips, the tray, and our own windows are excluded.
- [x] **SHOW window + capture target.** Tray double-click or right-click → SHOW opens a settings
  dialog listing processes with any visible window (child windows included). The choice is saved
  and shared by recording and casting; `wfm.exe` is excluded.
- [ ] **Confirm MPC-HC video capture.** With the target set to `mpc-hc64.exe`, check the log for
  `window: dualpistols.mp4`, `content: visible|black`, and `method: screen|printwindow`. If both
  methods are black, GDI cannot see the player's GPU surface and we need another capture path.
- [ ] **Implement the cast pipeline.** Capture/encode reuse the same target; feed H.264 to the
  existing MPEG-TS/UDP sender and validate with VLC on a client.
- [ ] **Validate the tray recorder on-device.** Run `record_5s.exe` in Winlator with a windowed
  app open, preview the MKV, and send the `.mkv.log`. Confirm the top-most window is captured and
  whether COLORONCOLOR fixed the 245 ms capture run.
- [ ] **Tune for Quest 3.** Local GDI follow costs ~22.5 ms/frame at 720p24; on-device is the
  unknown. If capture is still slow, try native BitBlt plus software downscale or a smaller region.
- [ ] **Audio capture spike.** Identify the container's audio path and whether capture can be
  taken without a virtual device the user must configure by hand.
- [ ] **Measure the overhead baseline.** Capture the current game frame rate with and without a
  test hook attached. This number is the acceptance bar for the rest of the project.

## Phase 2 — Local recorder (goal 2)

- [ ] Implement capture → encode → MKV pipeline with a bounded queue (no unbounded buffering).
- [ ] Default the Save dialog to `D:\` (host `Downloads`) and confirm that path exists and is
  writable in at least two forks; fall back to the last used directory when it is not.
- [ ] Implement duration cap (configurable 10–15 min) with automatic stop and clean finalize.
- [ ] Add free-space and encoder-pressure checks that block or warn before a doomed recording.
- [ ] Validate: 15-minute clip at stable frame rate, flat memory, file plays in a standard player.

## Phase 3 — Tray shell (goal 4)

- [x] Background-resident process with tray icon, menu, and stub sessions. Replaced the temporary
  message box in `src/main.cpp`.
- [x] Tray menu: `SHOW` / `RECORD LOCAL` / `CAST LOCAL` / `EXIT` when idle;
  `STOP RECORDING` or `STOP CASTING` while active. `SHOW` opens the same actions in a window.
- [x] `RECORD LOCAL`: capture source screen, then a Save dialog in `D:\` pre-filled with
  `recording_YYYY-MM-DD_hhmmss.mkv`; start immediately on OK, no-op on Cancel.
- [x] `CAST LOCAL`: receiver IPv4 dialog prefilled with the last address, then the capture source
  screen. Persisted to `%APPDATA%\wxr-vid-cap\settings.ini`.
- [x] Capture source screen: top-most window (default), a running application from a list, or
  launch an EXE with the hook (`Recommended for D3D11/12 and Vulkan`).
- [x] Explicit session state machine (idle / recording / casting) so the two modes cannot overlap
  even if the UI is bypassed.
- [x] Stop-on-exit: finalize a local file (empty stubs delete themselves), stop the sender.
- [ ] Verify the tray icon, menus, and both dialogs inside a Winlator container (Windows-only so
  far).

## Phase 5 — In-process graphics hook capture (goal 1, primary path)

- [~] **Verify the hook on-device.** Run `RECORD LOCAL` or `CAST LOCAL` with a target selected, or
  `wxr-vid-cap.exe --hook-debug` for the 3-second diagnostic; it reports the injection method,
  source resolution, frame count, and writes `hook_debug.bmp`. If injection fails, the popup
  includes `wxr_hook.log`. Local validation: 64-bit direct injection, 159 frames in 5 s, correct
  color in the dumped frame.
- [ ] **Code signing / AV false positives.** The injector and hook DLL will be flagged by Defender
  heuristics. Plan Authenticode signing, a Microsoft false-positive submission, and keeping the
  probe out of the shipping package.
- [x] **D3D11 Present hook proof of concept.** MinHook on the live `Present` vtable entry, two
  staging textures for stall-free readback, 30 fps capture cap, frames published through
  `Local\wxrvidcap_frames`. Validated locally against `tools/d3d11_test.exe`.
- [~] **Chain-last hook management.** The detour calls the previous `Present`/`EndScene` pointer, so
  mod hooks keep running. Re-hooking after a vtable change is not implemented yet.
- [ ] **Synthetic pre-hook test.** Install a test hook before ours that tints the frame; verify the
  captured output is tinted and the app still renders correctly.
- [x] **Injection host.** `CreateRemoteThread` + `LoadLibrary` into the selected process, with
  `wxr_inject32.exe` as the 32-bit fallback. Verified locally against 64-bit and 32-bit targets.
- [x] **Shared-memory frame bridge.** Named mapping with two slots, seqlock, `enabled` flag so the
  hook costs one atomic read when capture is off.
- [x] **D3D9 EndScene hook** for EVR/WineD3D apps. Same DLL, vtable slot 42, system-memory readback.
- [x] **D3D10 capture.** Detects `d3d10.dll` and reads the swapchain backbuffer through
  `ID3D10Texture2D` staging instead of the D3D11 path. Verified with `cube_d3d10`.
- [x] **D3D12 capture.** `wxr_launch.exe` injects at launch; the hook captures `D3D12CreateDevice`
  and the device's command queue, then copies the backbuffer on the app's queue in `Present`.
  Verified with `cube_d3d12`. The container needs vkd3d-proton; without it `D3D12CreateDevice`
  returns `E_NOINTERFACE`.
- [x] **OpenGL capture.** Hooks `opengl32!wglSwapBuffers` and `gdi32!SwapBuffers`, reads the
  backbuffer with `glReadPixels`. Verified with `cube_gl`.
- [x] **Vulkan capture.** Launch injection hooks `vkCreateDevice`, `vkCreateSwapchainKHR` (adds
  `TRANSFER_SRC` usage) and `vkQueuePresentKHR`. Verified with `cube_vk`.
- [x] **D3D7 capture.** Hooks `IDirectDrawSurface7::Blt` and reads the source back buffer before
  the blit. Verified with `cube_d3d7` windowed and fullscreen.
- [x] **D3D8 capture.** Hooks `IDirect3DDevice8::Present` and reads the back buffer with
  `GetBackBuffer` + `LockRect`. The container's d3d8 is 32-bit, so the 32-bit pass uses
  `cube_d3d832.exe` and injects the 32-bit hook through `wxr_inject32.exe`.
- [ ] **Validate the eight test apps on-device.** Run `cube_runner.exe` once; it records every API
  windowed and fullscreen, in 64-bit and 32-bit builds, keeps the videos, and writes one
  `cube_tests.log`. Check each app's `testapp.log` for API/audio status and inspect each MKV for the
  renderer label. The 32-bit D3D8 pass is the one that matches the container's d3d8; D3D12 needs
  vkd3d-proton.
- [x] **Integrate with Recorder/Caster.** `Recorder` and `Caster` share the same hook-first capture
  path; the recorder writes MKV, the caster encodes the same frames and audio to MPEG-TS/UDP.
- [ ] **Safety:** only inject into the process the user picked; never into system/anti-cheat
  processes; fail closed with a clear error.

## Phase 6 — LAN cast (goal 3)

- [x] Write the stream contract: MPEG-TS over UDP, H.264 + Opus, port, packet sizing, and the exact
  `udp://@:<port>` string the viewer types into VLC. No recording side.
- [x] Implement an in-tree MPEG-TS packetizer (PAT/PMT/PCR/PES) with no per-frame allocation that
  grows over time. Structural tests in `tests/ts_muxer_tests.cpp`.
- [x] Implement the UDP sender with a configurable unicast target and explicit failure when the
  target is missing or unresolvable. Datagrams are capped at 7 packets (1316 bytes).
- [x] Enforce "cast writes nothing": no file handle, no temp mux file, no fallback recording.
- [x] Wire the muxer and sender into `Caster`: the same hook-first capture and OpenH264/Opus
  encoders as `Recorder`, with a monotonic 90 kHz clock. H.264 and Opus are muxed to MPEG-TS;
  the PMT carries the "Opus" registration and Opus audio descriptors, and audio PES payloads use
  the OpusTS syncword framing VLC requires. No receiver handshake: it keeps sending whether or
  not VLC has joined.
- [x] Validate the transport with VLC on Windows loopback: 5 s synthetic stream, VLC demuxed and
  decoded it to a 4.57 s MP4. Uses `tools/cast_test_source`.
- [x] Feed AAC ADTS frames too and confirm VLC plays audio: 4.74 s decoded to PCM, and the
  re-muxed MP4 contains both `avc1` and `mp4a` tracks.
- [x] Validate the real `Caster` on loopback (2026-11-12): a hooked D3D9 test app was captured and
  sent for 10 s; VLC 3.0.24 decoded the Opus track as two-channel audio alongside H.264.
- [x] Automatic source fallback: if the selected window closes, recording and casting try to
  re-hook the top-most application, then fall back to GDI top-most if the new hook produces no
  frames. The WXR Capture Tool shows the current source.
- [x] `SELECT CAST SOURCE...` while casting opens the capture source screen and switches the live
  stream without stopping it. Recording has no such menu item and switches automatically.
- [x] Scope the shared frame mapping to the target process ID, so a hook DLL left in an older
  application cannot feed frames into a later session.
- [ ] Validate with VLC on a phone and a PC over a real LAN: stable playback for 15+ minutes,
  latency under ~1 s with cache at 200–300 ms, game frame rate at the Phase 1 baseline.
- [ ] Confirm VLC can record the incoming stream with its own Record button, since that is the
  viewer's expected path to a file.

## Background budget

- [x] Baseline measured: 0 ms idle CPU over 10 s, 1.4 MB private, 7.0 MB working set, 128 handles,
  4 threads, 25 modules.
- [x] Delay-load `comdlg32` and `ws2_32` so idle does not map the Save dialog or Winsock.
- [x] Single-instance mutex; a second launch hands off to the running copy instead of starting.
- [ ] Re-measure after capture, encode, and cast land: idle CPU must stay at 0 ms and private bytes
  under ~2 MB, with no timers or polling added.

## Housekeeping

- [ ] Keep `PROJECT_GOALS.md` open questions current; move resolved answers into the relevant
  section instead of leaving them dangling.
- [x] Add a build/run section to the top-level `README.md` once Phase 0 lands.

# Standalone-Shadowplay — Milestone snapshot

Date: 2026-05-16
Status: **Architecture proven (no overlay, no App, no telemetry). Recording-trigger blocked at bus dispatch.**

## What you can run today

```
bin\probe.exe
```

A single 164 KB EXE that:

1. Spawns its own `NvContainer.exe -c -d plugins ...` (or detects an externally-running one)
2. NvContainer loads only **`nvspserviceplugin64.dll`** (and now `nvspcaps64.dll` as second plugin) — log shows both `Active, Started, Mandatory`
3. **NEVER spawns** `NVIDIA Overlay.exe`, `NVIDIA app.exe`, `NVIDIA app Backend.exe`, anything CEF, anything from the `NvBackend\` tree
4. **NEVER registers** an NVIDIA service, NEVER touches autostart, NEVER calls home (no winsock/winhttp/wininet imports in nvspapi64)
5. Loads `nvspapi64.dll`, calls `CreateShadowPlayApiInterface`, gets a real `IShadowPlayApi*` (vtable address at runtime exactly matches reversed IDA RVA `0x24E908`)
6. Joins the MessageBus, `GetStatus` round-trips → **S_OK**
7. Local-state methods (`RegisterCallback`, `ReleaseInterface`) → S_OK

This is the entire "no overlay, no NVIDIA App" half of the goal — provably done.

## Where the wall is, precisely

`CreateCaptureSession` (msg ID 263) and `EnableShadowPlay` (async post 0x80000034 + sync wait) hang past their own documented timeouts.

After reading **`ShadowPlayServicePlugin::ProcessReceived`** (RVA `0x1800324E0`), the truth:

```c
if ((v9 & 0xFFFFFFF0) != 0x80000030 || !*(QWORD*)(a1 + 216))
    return 0;  // "not my message" — silently drops
```

**The plugin only handles bus messages with IDs `0x80000030..0x8000003F`.** Anything else (including client-side msg IDs 257, 263, 258) — the plugin returns 0 (not handled). The bus dispatcher tries the next peer, finds none, the client blocks waiting for a reply that's never coming.

The plugin in NvContainer is **only a half of the server side**. The actual capture-session handler lives somewhere else. Most likely:

- The plugin's role is to receive `0x80000034`-class lifecycle messages and **spawn the actual SP server process** (`nvsphelper64.exe` or `nvspcaps64`-as-EXE-via-some-launcher) via `ShadowPlayController::LaunchSPServer` → `CreateProcessInSession`.
- `CreateProcessInSession` requires LocalSystem privileges to spawn into the user session. NvContainer's normal install runs as a LocalSystem service for exactly this reason.
- Our child-mode NvContainer runs as the user, so `LaunchSPServer` fails silently → SP server never starts → CreateCaptureSession has no peer → hangs.

We tested this: ran NvContainer as SYSTEM via a scheduled task — but that put NvContainer in session 0 while the probe is in session 1. The bus may be session-aware, or `CreateProcessInSession` needs both SYSTEM AND the target session ID, which schtasks-style SYSTEM can do but probe-side message routing apparently doesn't reach. Empirically, both child-mode and SYSTEM-session-0 modes hang the same way.

## What's actually known, verified by code+runtime

| Component | Status | Evidence |
|---|---|---|
| Installer extraction (no install) | ✅ | `tools\extract-installer.ps1`, 124 PE files indexed |
| Minimum runtime deploy | ✅ | `tools\deploy-runtime.ps1`, only ShadowPlay/NvVAD/MessageBus to canonical paths |
| Registry seed (Cfg2/Cfg3) | ✅ | `tools\seed-registry.ps1` — XOR-validated magic + client ID |
| NvContainer in child mode | ✅ | log: `ShadowPlayContainerPlugin — Active, Started, Mandatory` |
| Both plugins loaded | ✅ | nvspserviceplugin64 AND nvspcaps64 both active |
| Reversed IShadowPlayApi vtable | ✅ | `src\IShadowPlayApi.h` |
| Standalone factory call | ✅ | probe prints real iface ptr, vtable RVA matches |
| MessageBus connectivity | ✅ | `GetStatus → S_OK` |
| Local-only methods | ✅ | RegisterCallback / ReleaseInterface → S_OK |
| **CreateCaptureSession** | ❌ | hangs — no bus peer handles msg ID 263 |
| **EnableShadowPlay** | ❌ | hangs — plugin can't `LaunchSPServer` without LocalSystem-in-session-1 |
| Tray icon + RegisterHotKey | ⏳ | trivial once recording starts |
| Clip file produced | ❌ | blocked on above |

## Two realistic next-session paths

### Path A2 — finish the extraction approach
1. **Use a real PsExec-style binary** to run NvContainer as SYSTEM **in the active user session**. `psexec -s -i 1` from Sysinternals (legitimate Microsoft tool). With NvContainer in session-1 as SYSTEM, the plugin's `LaunchSPServer` should succeed.
2. If SP server launches, EnableShadowPlay's polling loop completes and the sync wait returns. CreateCaptureSession then has a peer.
3. From there, wire `CaptureSessionControl(Start)`, `FrameCaptureControl` (screenshot), `RegisterCallback` for hotkey delivery, build the tray-icon EXE.

### Path D — pivot to clean-room
Drop the NVIDIA App-stack extraction entirely. Build a real clipper using:
- **NVENC** via the already-installed `nvEncodeAPI64.dll` (NVIDIA's official public encoder API, fully documented in their Video Codec SDK)
- **DXGI Output Duplication** for desktop capture (Microsoft public API)
- **WASAPI loopback** for system audio
- **Ring buffer** of the last N seconds of NVENC bitstreams in RAM, flushed to fragmented MP4 on hotkey
- **Tray icon + `RegisterHotKey`** as before

Output: a single ~5 MB EXE, no NVIDIA App involvement at all, no fragility against driver updates, no NvContainer, no MessageBus. Higher engineering cost (a few thousand lines of C++), but it sidesteps every remaining unknown.

This is what was originally Option A in the architecture-selection question at the start; the user chose Option B (the extraction approach). After hitting the dispatch wall, **D is the safer route to a working clipper**.

## Files of record (state survives across sessions)

- `src\IShadowPlayApi.h` — reversed 20-slot CShadowPlayApi vtable
- `src\probe.cpp`, `src\build.bat` — the working harness
- `tools\extract-installer.ps1` / `seed-registry.ps1` / `deploy-runtime.ps1` / `run-as-system.ps1`
- `analysis\dump_methods.out`, `dump_inner2.out`, `dump_dispatch.out` — IDA pseudocode for every relevant function
- `analysis\dump_dispatch.out` — **the critical `ProcessReceived` decompile that nailed why the hang happens**
- `notes\architecture.md` — full RE map
- `bin\nvcontainer\nvc.log` — fresh after each run

## Reproduce from a clean checkout

```powershell
.\tools\extract-installer.ps1 -InstallerPath C:\path\to\NVIDIA_app_v11.0.7.247.exe
.\tools\seed-registry.ps1           # elevated
.\tools\deploy-runtime.ps1 -Force   # elevated
cmd /c .\src\build.bat
.\bin\probe.exe
```

## Honest assessment

**The "no overlay, no NVIDIA App" part of the goal is real and proven.** Run `bin\probe.exe`. Watch Process Explorer. The only NVIDIA process that ever starts is our own child-mode NvContainer.

**The "actually clip" part of the goal needs another session.** Either fix the privilege/session model (Path A2 — likely 1 session) or pivot to the clean-room approach (Path D — likely 2-3 sessions, but produces a more robust product).

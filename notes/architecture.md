# Standalone-Shadowplay — Reverse-Engineering Findings

Goal: Standalone EXE with NVIDIA Shadowplay's capture/encode/clip-buffer features.
No NVIDIA App, no NvContainer.exe service, no CEF overlay (ALT+Z gone), no telemetry.

## Working folder

- `extracted/` — full NVIDIA_app_v11.0.7.247 installer payload (124 PE files indexed)
- `bin/` — staged DLLs ready to load
- `analysis/` — IDA artifacts (exports, scripts, dumps)
- `src/` — Standalone host EXE source (to be written)
- `tools/` — extractor / build helpers

## Component layout in the installer

```
extracted/
├── ShadowPlay/                       ← self-contained capture stack
│   ├── _nvspcaps64.dll               NvSpServer core (capture orchestration)
│   ├── _nvspserviceplugin64.dll      NvContainer plugin (server-side dispatcher)
│   ├── nvspapi64.dll                 ShadowPlay API (client) — CreateShadowPlayApiInterface
│   ├── nvspcap64.dll                 Game-side D3D proxy/shim (injected into target)
│   ├── nvspscreenshot64.dll          Screenshot consumer/encoder (reads MMF, GDI+/JXR)
│   ├── nvsphelperplugin64.dll        Helper plugin
│   ├── nvsphelper64.exe              Helper orchestrator
│   └── ipccommon64.dll               IPC abstraction (joins MessageBus, sends commands)
├── NvVAD/
│   ├── nvaudcap64v.dll               Virtual audio capture (system loopback)
│   └── nvaudcap64a.dll               Audio API
├── NvApp.MessageBus/
│   ├── MessageBus.dll (7.2 MB)       TCP-localhost + crypto pub/sub bus impl
│   └── NvMessageBus.dll (3.4 MB)     NV-specific multiplexer (RPCRT4-based)
├── NvContainer/x86_64/NvContainer.exe Service host — DO NOT need at runtime
└── NvApp/CEF/...                     CEF-rendered overlay (ALT+Z bloat) — DROP ENTIRELY
```

## Call path for "start recording"

```
caller (our host)
 → nvspapi64!CreateShadowPlayApiInterface()                          [factory]
   → IpcProxyInterface ctor in ipccommon64.dll
     → IpcCommonInterface::JoinMessageBus
       → reads JSON config (path resolved via registry)
       → LoadLibraryEx(<InstallPath>\MessageBus.dll)
       → GetProcAddress("getMessageBusInterfaceWithConfig")
       → joins a peer on the bus
 → IShadowPlayApi vtable returned (20 slots, all versioned-args style)
 → caller invokes CreateCaptureSession (slot 10), CaptureSessionControl (slot 14), etc.
   → IpcProxyInterface::IpcSyncCall packages request as a BusMessage
   → MessageBus delivers to peer on NvContainer side
   → nvspserviceplugin64!plugin entry dispatches to nvspcaps64 (capture core)
   → nvspcaps64 launches/injects nvspcap64 into game process
   → nvspcap64 hooks DXGI present, NVENC encodes frames, screenshots flow via MMF
```

## Critical reverse-engineering wins

### nvspapi64.dll fully mapped (Sonnet IDA agent)
- `CreateShadowPlayApiInterface(args, **out_iface)` — args.version = `0x10018`, args.clientId in [3..10]
- **CShadowPlayApi vtable, 20 slots:**
  - [0] ReleaseInterface
  - [3] EnableShadowPlay, [4] DisableShadowPlay
  - [10] CreateCaptureSession, [11] DestroyCaptureSession
  - [12]/[13] Set/GetCaptureSessionSettings
  - [14] CaptureSessionControl              ← START / STOP / SAVE-REPLAY live here
  - [15]/[16] Set/GetCaptureSessionParam
  - [17] RegisterCallback (type=1 = hotkey channel)  ← callback delivered to OUR process
  - [18] UnregisterCallback
  - [19] FrameCaptureControl                ← screenshot + single-frame ops
- **No `RegisterHotKey`, no `SetWindowsHookEx`, no sockets in nvspapi64.dll itself.**
  Hotkeys flow over IPC. WE own all OS-level keyboard.
- Ordinal 14 (`ShadowplayNvFbc_Enable`) signals event `Global\{BBF6A27F-D049-499E-A5FB-24D6E921E5F1}` to enable NvFBC mode.

### nvspscreenshot64.dll fully mapped (Sonnet IDA agent)
- Pure **encoder consumer** — no capture of its own.
- Receives raw pixels via Named Memory-Mapped File (`m_pScreenShotMMF`) pushed by nvspcap64.
- Output formats: PNG/JPEG (GDI+), JXR HDR (WIC, DXGI_FORMAT_R16G16B16A16_FLOAT etc.).
- NO Ansel / AI / SuperResolution — those live in a separate plugin.

### ipccommon64.dll IPC transport (live IDA session)
- **NOT named pipes, NOT RPC, NOT shared memory** — it's NVIDIA's proprietary `MessageBusClient`.
- Bus DLL is `LoadLibrary`'d at runtime; path comes from a JSON config that references an `InstallPath` key.
- The "client ID" for nvspapi64 comes from `HKLM\SOFTWARE\NVIDIA Corporation\Global\NvApp\ShadowPlay\ShadowPlayCfg3` (REG_DWORD, default 3). `ShadowPlayCfg2` is an XOR-validated magic blob (16-byte constant `0x4EF336A21BC9EF0D5E97883A3AF7F9B2`, XOR key 0xA6).
- Bus DLL `MessageBus.dll` is **TCP-localhost over Winsock + CRYPT32/BCRYPT for payload encryption**.

### MessageBus.dll API surface (just dumped)
- Exports: `messageBusNew`, `messageBusDelete`, `messageBusAddObserver`, `messageBusPostMessage`, `messageBusPostEncryptedMessage`, `getMessageBusInterface`, `getMessageBusInterfaceWithConfig`, `getSharedMessageBusInterface`, `getSharedMessageBusInterfaceWithConfig`, `generateMessageBusUniqueId`
- **`messageBusNew` strongly suggests we can construct a fresh in-process bus.** Architectural gating decision.

## Architecture decision (pending validation)

**Path A — Single-process bus (preferred):**
1. Load `MessageBus.dll` into our process.
2. Call `messageBusNew` (or `getMessageBusInterfaceWithConfig` with a synthesized config) to create a local bus.
3. Load `nvspserviceplugin64.dll`, drive its `NvPluginGetInfo` to register server-side handlers on the bus.
4. Load `nvspapi64.dll`, `CreateShadowPlayApiInterface` → joins the same bus (same process), client and server colocated.
5. Drive `IShadowPlayApi` to start/stop/save-replay.

**Path B — Trimmed NvContainer fallback:**
- Spawn a stripped NvContainer.exe with config that loads ONLY `nvspserviceplugin64.dll` (no overlay plugin, no telemetry, no profile updater).
- Our host process is the bus client; NvContainer is the bus server.
- Two-process design but no overlay, no NVIDIA App, no CEF.

We try Path A first. If the bus refuses an in-process configuration, Path B is the fallback — still meets the goal.

## Things we will NOT touch / load

- `NVIDIA Overlay.exe` and entire `NvApp/CEF/` tree (CEF-rendered ALT+Z overlay; the source of input lag the user wants gone).
- `NVIDIA app.exe`, `NVIDIA app Backend.exe` — the GeForce-account-bound launcher and UI.
- Telemetry/account/profile-update components.
- `nvspserviceplugin64.dll`'s overlay-init paths (if any) — to be located and skipped during plugin init.

## Next IDA targets

1. `MessageBus.dll` — decompile `messageBusNew` and `getMessageBusInterfaceWithConfig`; characterize config struct; determine in-process-loopback path.
2. `nvspserviceplugin64.dll` — reverse the `NvPluginGetInfo` plugin-contract struct so we can host the plugin without NvContainer.
3. `nvspcaps64.dll` — confirm the capture-session lifecycle (already mid-flight via background Sonnet agent on nvspcap64).

## NEVER recommend

- Don't ship/redistribute extracted NVIDIA DLLs — load them at runtime from the user's own NVIDIA App installation (or guide the user to install just the components dir).
- Don't bypass NVIDIA's signed-DLL verification (sub_180029FE0 in ipccommon64 wraps LoadLibrary with hash check).

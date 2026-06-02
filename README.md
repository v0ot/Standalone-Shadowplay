# Standalone ShadowPlay

Use NVIDIA ShadowPlay's instant replay and clipping without the NVIDIA App or its overlay.

Same hardware-accelerated capture (NvFBC + NVENC), same game detection, same clip quality — just without the bloat.

## What you get

- **169 KB tray app** — zero CPU when idle
- **Instant replay** — saves the last N seconds on hotkey
- **Per-game clip folders** — Deadlock, Overwatch, etc.
- **No NVIDIA App, no overlay, no CEF, no telemetry**
- **Anticheat safe** — only NVIDIA-signed binaries touch game processes

## Hotkeys

| Key | Action |
|-----|--------|
| `Alt+F10` | Save instant replay |
| `Alt+F9` | Toggle manual recording |
| `Alt+Shift+F10` | Toggle instant replay on/off |

Right-click the tray icon for menu + settings.

## Install

### Requirements

- NVIDIA GPU
- NVIDIA display driver installed via [NVCleanstall](https://www.techpowerup.com/download/techpowerup-nvcleanstall/) with **ShadowPlay** + **Virtual Audio** checked:

![NVCleanstall — check ShadowPlay + Virtual Audio](docs/nvcleanstall.png)

### Steps

1. [**Download ZIP**](https://github.com/v0ot/Standalone-Shadowplay/archive/refs/heads/master.zip) and extract it
2. Right-click **`install.bat`** → **Run as administrator**

That's it. The installer creates a desktop shortcut, sets ShadowPlay to start
with Windows, and launches it for you.

### Uninstall

```
install.bat /uninstall
```

## Settings

Double-click the tray icon or right-click → Settings:

- **Resolution** — capture width/height
- **FPS** — 1–120
- **Buffer length** — instant replay duration in seconds
- **Encoder profile** — Base / Main / High
- **Output / Input device** — pick which speakers/mic to record (defaults to your
  current Windows devices)
- **Microphone** — on/off (toggles mic in clips only, stays live for Discord)

## FAQ

**Do I need the NVIDIA App?**
No. That's the whole point.

**Do I need ShadowPlay checked in NVCleanstall?**
Yes — it installs the driver-level capture infrastructure (nvspcap64.dll, NvContainer service). The overlay and app are replaced by this project.

**Do I need Virtual Audio checked?**
Yes — required for system audio in clips. Without it, clips will have no audio.

**Does this work with Vanguard / EAC / BattlEye?**
Yes. The D3D hook (nvspcap64.dll) is NVIDIA-signed and whitelisted by all major anticheats.

**What about audio?**
System audio is captured automatically. Mic can be toggled via the tray menu.

**What about stretch resolution?**
Handled automatically — NvFBC captures at the GPU output level.

**Instant Replay says it's "paused" / clips won't save?**
ShadowPlay (by NVIDIA's design) refuses to record while **DRM-protected video**
is on screen — Netflix, Spotify, or DRM-protected YouTube in a browser. Close
that tab/app and Instant Replay resumes automatically within ~20 seconds. This
only affects desktop capture; capturing a focused game is unaffected.

**Does it start automatically?**
Yes — the installer adds it to Windows startup. Right-click the tray icon → Quit
to stop it, or run `install.bat /uninstall` to remove startup entirely.

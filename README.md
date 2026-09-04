<p align="center">
  <strong>╔══════════════════════════════════════╗</strong><br>
  <strong>║        CAGT — AMD GPU Fan Control    ║</strong><br>
  <strong>╚══════════════════════════════════════╝</strong>
</p>

<h1 align="center">CAGT</h1>

<p align="center">
  <strong>A single-file, TUI AMD GPU fan controller for Linux</strong><br>
  Just you and your GPU cooled properly.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Version-v1.3.1-blue?style=flat-square" alt="Version">
  <img src="https://img.shields.io/badge/Language-C-A8B9CC?style=flat-square&logo=c" alt="C">
  <img src="https://img.shields.io/badge/TUI-ncurses-FFB6C1?style=flat-square" alt="ncurses">
  <img src="https://img.shields.io/badge/License-IRX_1.0-yellow?style=flat-square" alt="License">
</p>

---

# What is CAGT?

CAGT is a single C file, TUI-based fan controller for AMD GPUs on Linux.

It reads your GPU temperature directly from `hwmon`, runs it through a fan curve, and writes the correct PWM value. It also runs as a background daemon with a clean CLI for remote control.

---

# Quick Start

```bash
gcc -std=c99 -O2 -o cagt cagt.c -lncurses
chmod +x cagt
sudo ./cagt
```

For daemon mode (example):

```bash
sudo ./cagt --daemon
./cagt status
./cagt fan 75
./cagt quit
```

---

# Development

If you're modifying CAGT, compile with stricter warnings to catch issues early:

```bash
gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -Wanalyzer-file-leak -o cagt cagt.c -lncurses
```

The production build doesn't need these flags, but they help catch bugs during development.

---

# TUI Controls

| Key | Action |
|------|--------|
| `1 – 9` | Select GPU (multi-GPU systems) |
| `M` | Toggle Manual / Auto mode |
| `T` | Cycle temperature unit (`°C → °F → K`) |
| `S` | Cycle temp sensor (Edge / Junction / Memory) |
| `C` | Cycle fan curve (Default / Quiet / Aggressive / Linear / Custom) |
| `R` | Reset peak temperature |
| `+ / -` | Adjust fan by ±5% (enters Manual) |
| `↑ / ↓` | Adjust fan by ±1% (enters Manual) |
| `0` | Set fan to 0% instantly |
| `Q` | Quit (restores auto fan control) |

All manual adjustments auto-switch you into Manual mode. Press `M` to go back to the curve.

---

# Daemon CLI

| Command | Action |
|---------|--------|
| `cagt status` | Show current temperature, fan speed, RPM, and mode |
| `cagt gpu <1-N>` | Select active GPU (multi-GPU systems) |
| `cagt fan <0-100>` | Set manual fan speed |
| `cagt fan auto` | CAGT controls fan via curve |
| `cagt fan gpu` | GPU hardware controls fan |
| `cagt curve <name\|0-4>` | Select curve (default, quiet, aggressive, linear, custom) |
| `cagt curve list` | List available curves |
| `cagt log <interval>` | Set logging interval (1s, 5s, 10s, 500ms, off) |
| `cagt quit` | Stop daemon |

The daemon forks to the background on start. It communicates via a Unix socket. If the daemon is running and you launch `cagt` without arguments, it will tell you. If the daemon is not running and you use a command, it will tell you to start it.

---

# Features

- Clean `ncurses` TUI — color-coded temps, fan curve visualization, RPM display
- Background daemon mode with Unix socket CLI
- Multi-GPU support — tabbed title bar, per-GPU independent state
- 5 fan curves — Default, Quiet, Aggressive, Linear, and Custom
- Custom fan curves via config file (`~/.config/cagt/custom_curve.conf`)
- Multi-sensor — Edge, Junction, Memory temperature where available
- Manual override — granular 1% or 5% steps, plus instant presets
- Temperature units — Celsius, Fahrenheit, or Kelvin
- Peak temperature tracking with reset
- Zero RPM detection
- Runtime counter
- Optional CSV logging (`--log` flag)
- Persistent settings per GPU (saved to `~/.config/cagt/config.conf`)
- RDNA3 `fan_curve` interface support
- ppfeaturemask detection with specific RDNA3 warnings
- Safe exit with verified auto-restore — handles known `pwm1_enable` quirks
- 25+ GPU detection — Polaris, Vega, Navi, RDNA3, and older GCN
- Single file — no headers, no build system, no dependencies beyond `ncurses`

---

# Custom Fan Curve

Create `~/.config/cagt/custom_curve.conf`:

```
# Format: temperature_c fan_percent
20 0
40 20
55 35
65 50
75 70
85 90
100 100
```

At least 2 points required. Temperatures must be ascending. Select it with `cagt curve custom` or cycle to it in the TUI with `C`.

---

# Default Fan Curve

| Temp (°C) | Fan Speed |
|-----------|-----------|
| 20 | 0% |
| 35 | 20% |
| 50 | 30% |
| 60 | 45% |
| 70 | 60% |
| 80 | 75% |
| 90 | 90% |
| 100 | 100% |

Also available: Quiet, Aggressive, and Linear curves. Temperatures between points are linearly interpolated.

---

# Supported GPUs

| Generation | Models |
|------------|---------|
| Polaris | RX 560, RX 570, RX 580 (including 2048SP) |
| Vega | RX Vega 56, Vega 64, Radeon VII |
| Navi 10 | RX 5600 XT, RX 5700, RX 5700 XT |
| Navi 20 | RX 6700 XT, RX 6800, RX 6800 XT, RX 6900 XT |
| Navi 30 (RDNA3) | RX 7600, RX 7700 XT, RX 7800 XT, RX 7900 XT, RX 7900 XTX |
| Older GCN | HD 7950, HD 7970, R9 290/390, R9 Fury |

Unknown AMD GPUs still work, they just show their PCI device ID.

---

# RDNA3 Requirements

RDNA3 GPUs (RX 7000 series) require the amdgpu overdrive bit to be enabled for manual fan control. If you see a warning about `ppfeaturemask`, add this to your kernel cmdline or `/etc/modprobe.d/amdgpu.conf`:

```
amdgpu.ppfeaturemask=0xffffffff
```

Then reboot.

---

# Dependencies

CAGT requires only `ncurses` for its TUI.

```bash
# Artix / Arch
sudo pacman -S ncurses

# Debian / Ubuntu
sudo apt install libncurses-dev

# Fedora
sudo dnf install ncurses-devel

# Void
sudo xbps-install ncurses-devel
```

---

# Project Structure

```text
CAGT/
├── cagt.c           # the entire program
├── LICENSE          # IRX License 1.0
├── CONTRIBUTING     # contribution guidelines
└── README.md        # this file
```

---

# Why CAGT Exists

CAGT is a small personal project.

It's a single static binary that runs in your terminal or as a daemon, updates at 10Hz, and exits cleanly, restoring your GPU to automatic control every time.

> Do one thing and do it well.

---

# Contributing

Contributions welcome.

Keep it simple. CAGT is intentionally a single-file tool with no external dependencies beyond `ncurses`.

Please read [CONTRIBUTING](CONTRIBUTING) for guidelines.

---

# License

Licensed under the [IRX License 1.0](LICENSE) © [realvolk](https://github.com/realvolk) 2026.
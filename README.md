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
  <img src="https://img.shields.io/badge/Version-v1.2.0-blue?style=flat-square" alt="Version">
  <img src="https://img.shields.io/badge/Language-C-A8B9CC?style=flat-square&logo=c" alt="C">
  <img src="https://img.shields.io/badge/TUI-ncurses-FFB6C1?style=flat-square" alt="ncurses">
  <img src="https://img.shields.io/badge/License-Volk_Open_License_1.0-yellow?style=flat-square" alt="License">
</p>

---

# What is CAGT?

CAGT is a single C file, TUI-based fan controller for AMD GPUs on Linux.

It reads your GPU temperature directly from `hwmon`, runs it through a fan curve, and writes the correct PWM value, all from a clean, interactive terminal interface.

---

# Quick Start

```bash
gcc -std=c99 -O2 -o cagt cagt.c -lncurses
chmod +x cagt
sudo ./cagt
```

---

# Controls

| Key | Action |
|------|--------|
| `1 – 9` | Select GPU (multi-GPU systems) |
| `M` | Toggle Manual / Auto mode |
| `T` | Cycle temperature unit (`°C → °F → K`) |
| `S` | Cycle temp sensor (Edge / Junction / Memory) |
| `C` | Cycle fan curve (Default / Quiet / Aggressive / Linear) |
| `R` | Reset peak temperature |
| `+ / -` | Adjust fan by ±5% (enters Manual) |
| `↑ / ↓` | Adjust fan by ±1% (enters Manual) |
| `0` | Set fan to 0% instantly |
| `Q` | Quit (restores auto fan control) |

All manual adjustments auto-switch you into Manual mode.

Press `M` to go back to the curve.

---

# Features

- Clean `ncurses` TUI — color-coded temps, fan curve visualization, RPM display
- Multi-GPU support — tabbed title bar, per-GPU independent state
- Auto fan curves — 4 profiles: Default, Quiet, Aggressive, Linear
- Multi-sensor — Edge, Junction, Memory temperature where available
- Manual override — granular 1% or 5% steps, plus instant presets
- Temperature units — Celsius, Fahrenheit, or Kelvin
- Peak temperature tracking with reset
- Zero RPM detection
- Runtime counter
- RDNA3 `fan_curve` interface support
- Safe exit with verified auto-restore — handles known `pwm1_enable` quirks
- 25+ GPU detection — Polaris, Vega, Navi, RDNA3, and older GCN
- Single file — no headers, no build system, no dependencies beyond `ncurses`

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

Also available: Quiet, Aggressive, and Linear curves.

Temperatures between points are linearly interpolated.

Curves are hardcoded in `fan_curve()`. Edit the `points` arrays and recompile to customize.

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
├── LICENSE          # Volk Open License 1.0
└── README.md        # this file
```

---

# Why CAGT Exists

CAGT is a small personal project.

It's a single static binary that runs in your terminal, updates at 10Hz, and exits cleanly, restoring your GPU to automatic control every time.

> Do one thing and do it well.

---

# Contributing

Contributions welcome.

Keep it simple. CAGT is intentionally a single-file tool with no external dependencies beyond `ncurses`.

Please read [CONTRIBUTING.md](CONTRIBUTING) for guidelines.

---

# License

Licensed under the [Volk Open License 1.0](LICENSE) © [realvolk](https://github.com/realvolk) 2026.
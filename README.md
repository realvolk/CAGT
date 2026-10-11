<h1 align="center">CAGT</h1>

<p align="center">
  <strong>AMD Fan Control</strong><br>
  hwmon • PWM • Fan curves • Daemon
</p>

<p align="center">
  Small enough to read in one sitting.<br>
  Complete enough to actually cool your GPU.
</p>

<p align="center">
  <a href="#what-it-is">What it is</a>
  •
  <a href="#features">Features</a>
  •
  <a href="#quick-start">Quick start</a>
  •
  <a href="#daemon">Daemon</a>
  •
  <a href="#fan-curves">Fan curves</a>
  •
  <a href="#safety-model">Safety model</a>
  •
  <a href="#requirements">Requirements</a>
  •
  <a href="#supported-gpus">Supported GPUs</a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-v1.4.0-blue?style=flat-square" alt="Version">
  <img src="https://img.shields.io/badge/license-CLEAR_v1-yellow?style=flat-square" alt="License">
  <img src="https://img.shields.io/badge/targets-AMD_RDNA_·_Navi_·_Vega_·_Polaris-informational?style=flat-square" alt="Targets">
</p>

---

## What it is

CAGT is a fan controller for AMD GPUs on Linux. It reads the GPU
temperature from `hwmon`, runs it through a fan curve, and writes the
resulting PWM value back. It runs either as a `ncurses` TUI or as a
background daemon with a CLI.

## Features

- `ncurses` TUI with color-coded temperatures and fan curve preview
- Background daemon with a Unix socket CLI
- Multi-GPU: tabbed title bar, per-GPU independent state
- Three control modes: curve, manual, hardware
- Five fan curves: Default, Quiet, Aggressive, Linear, Custom
- Custom curves via `~/.config/cagt/custom_curve.conf`
- Multi-sensor: Edge, Junction, Memory where exposed by the driver
- Manual override with 1% and 5% steps, plus instant presets
- Temperature units: Celsius, Fahrenheit, Kelvin
- Peak temperature tracking with reset
- Zero RPM detection
- Runtime counter
- Optional CSV logging
- Persistent per-GPU settings in `~/.config/cagt/config.conf`
- RDNA3+ `fan_curve` interface support via `gpu_od/fan_ctrl/fan_curve`
- `ppfeaturemask` detection with RDNA3+ warnings
- Safe exit with verified auto-restore, including `pwm1_enable` retry
- GPU detection table covering Polaris, Vega, Navi, RDNA3, RDNA4, and older GCN

## Quick start

### Build

```sh
gcc -std=c99 -O2 -o cagt cagt.c -lncurses
chmod +x cagt
```

For development:

```sh
gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -Wno-format-truncation -o cagt cagt.c -lncurses
```

The production build does not need these flags.

### Run

```sh
sudo ./cagt
```

Or as a daemon:

```sh
sudo ./cagt --daemon
./cagt status
./cagt fan 75
./cagt quit
```

Root is required. CAGT writes to `/sys/class/drm/.../hwmon/.../pwm1`
and reads the corresponding temperature and RPM files.

### TUI controls

| Key | Action |
|------|--------|
| `1 – 9` | Select GPU (multi-GPU systems) |
| `M` | Cycle mode (curve / manual / hardware) |
| `T` | Cycle temperature unit (`°C → °F → K`) |
| `S` | Cycle temp sensor (Edge / Junction / Memory) |
| `C` | Cycle fan curve |
| `R` | Reset peak temperature |
| `+ / -` | Adjust fan by ±5% (enters manual) |
| `↑ / ↓` | Adjust fan by ±1% (enters manual) |
| `0` | Set fan to 0% instantly |
| `Q` | Quit (restores auto fan control) |

Manual adjustments auto-switch into manual mode.

## Daemon

`cagt --daemon` forks to the background and listens on a Unix socket.
The CLI talks to that socket. If the daemon is not running and you
issue a command, it tells you to start it. If the daemon is running
and you launch `cagt` with no arguments, it tells you so.

Socket path: `$XDG_RUNTIME_DIR/cagt.sock` if set, otherwise
`/tmp/cagt-$UID/sock` in a `0755` directory.

| Command | Action |
|---------|--------|
| `cagt status` | Show temperature, fan speed, RPM, mode |
| `cagt gpu <1-N>` | Select active GPU |
| `cagt fan <0-100>` | Set manual fan speed |
| `cagt fan auto` | CAGT controls the fan via curve |
| `cagt fan gpu` | GPU hardware controls the fan |
| `cagt curve <name\|0-4>` | Select curve |
| `cagt curve list` | List available curves |
| `cagt log <interval>` | Set logging interval (`1s`, `5s`, `10s`, `500ms`, `off`) |
| `cagt quit` | Stop the daemon |

## Fan curves

Curves map temperature to fan speed. Between points, values are
linearly interpolated. Five curves ship with CAGT.

### Default

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

Also available: Quiet, Aggressive, Linear.

### Custom

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

At least two points. Temperatures must be ascending. Select with
`cagt curve custom` or cycle to it in the TUI with `C`.

On RDNA3+, CAGT writes curve points directly to
`gpu_od/fan_ctrl/fan_curve` and lets the hardware interpolate. On
older GPUs, CAGT recomputes the curve each tick and writes PWM.

## Safety model

CAGT writes to `/sys`. If it dies without restoring auto mode, the GPU
firmware is left wherever the last PWM write put it. Every exit path
is designed around that.

| Layer | Mechanism | Verified by |
|---|---|---|
| CAGT → `pwm1` | Write on tick, only on change | read-back optional |
| CAGT → `pwm1_enable` on start | Set to `1` (manual) | checked after write |
| CAGT → `pwm1_enable` on exit | Set to `2` (auto) | re-read and verified, retried on failure |
| RDNA3+ exit | `fan_curve` reset to `r`, then `pwm1_enable=2` | read-back on `pwm1_enable` |
| Crash handler | `SIGINT`, `SIGTERM` set flag, loop restores | async-signal-safe |
| Daemon → client | Unix socket | `SO_PEERCRED` uid check |
| `custom_curve.conf` | Parsed at load | rejected if fewer than 2 points or non-ascending temps |
| `config.conf` | Parsed at load | per-GPU state, unknown keys ignored |

Zero RPM is detected and treated as valid. CAGT does not force the fan
on below the curve's first non-zero point.

Not covered: CAGT does not stop the amdgpu driver from overriding
`pwm1_enable` if `ppfeaturemask` is not set on RDNA3+. It warns on
startup. It does not stop you from setting 0% manually and walking
away. It does not persist across reboots — the daemon has to be
started again.

## Requirements

Always:

- `gcc` or `clang`
- `ncurses` development headers
- Linux with `hwmon` fan control exposed by amdgpu

By feature:

| Feature | Extra |
|---|---|
| Manual fan on RDNA3+ | `amdgpu.ppfeaturemask=0xffffffff` in kernel cmdline |
| Custom curve | `~/.config/cagt/custom_curve.conf` |
| Daemon mode | nothing extra |
| CSV logging | `--log` flag |

Install `ncurses`:

```sh
# Artix / Arch
sudo pacman -S ncurses

# Debian / Ubuntu
sudo apt install libncurses-dev

# Fedora
sudo dnf install ncurses-devel

# Void
sudo xbps-install ncurses-devel
```

### RDNA3+ note

RX 7000 and RX 9000 series GPUs require the amdgpu overdrive bit for
manual fan control. Add this to your kernel cmdline or
`/etc/modprobe.d/amdgpu.conf`:

```
amdgpu.ppfeaturemask=0xffffffff
```

Reboot. If CAGT prints a `ppfeaturemask` warning on startup, this is
why.

## Supported GPUs

| Generation | Models |
|------------|---------|
| Polaris | RX 560, RX 570, RX 580 (incl. 2048SP) |
| Vega | RX Vega 56, Vega 64, Radeon VII |
| Navi 10 | RX 5500 XT, RX 5600 XT, RX 5700, RX 5700 XT |
| Navi 20 | RX 6300, RX 6400, RX 6500 XT, RX 6600, RX 6600 XT, RX 6650 XT, RX 6700, RX 6700 XT, RX 6800, RX 6800 XT, RX 6900 XT, RX 6950 XT |
| Navi 30 (RDNA3) | RX 7600, RX 7700 XT, RX 7800 XT, RX 7900 XT, RX 7900 XTX |
| Navi 40 (RDNA4) | RX 9060 XT, RX 9070, RX 9070 XT |
| Older GCN | HD 7950, HD 7970, R9 290/390, R9 Fury |

Unknown AMD GPUs with a `hwmon` fan interface still work. They show
their PCI device ID instead of a name.

## Project structure

```text
CAGT/
├── cagt.c           # the entire program
├── LICENSE          # IRX License 1.0
├── CONTRIBUTING     # contribution guidelines
└── README.md        # this file
```

## Contributing

Contributions welcome. CAGT is intentionally a single file with no
dependencies beyond `ncurses`. Keep it that way.

See [CONTRIBUTING](CONTRIBUTING) for guidelines.

## License

CLEAR v1 license. See [LICENSE](LICENSE).
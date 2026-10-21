# Fan Curve

A small, safe, free fan controller for Apple Silicon Macs — a native menu-bar
replacement for Macs Fan Control. Shows CPU/GPU temperature and fan speeds in the
menu bar, with Auto / Manual / Curve control in a click-down popover.

I built it because I like a laptop that stays cool to the touch, and I didn't want
to keep paying for a fan app to get that. It runs every day on my 14-inch M1 Pro
MacBook Pro (MacBookPro18,3, macOS 26), alongside its sister project
[Battery Tool](https://github.com/noahberry01123/battery-tool), which moves everyday apps onto the
efficiency cores. The story behind both: [Running My M1 Pro on Its Efficiency Cores]({{MEDIUM_ARTICLE_URL}}).

> **Use at your own risk.** This is a personal project that writes to the SMC (the
> chip that controls the fans). Apple's firmware still protects the chip, but the
> software comes with no warranty and the author accepts no liability (see
> [LICENSE](LICENSE)). It is not affiliated with Apple or with Macs Fan Control.

**Safety:** every target speed is clamped to each fan's own `F<n>Mn`/`F<n>Mx` range as
reported by the SMC. If those limits can't be read or don't make sense, nothing is
written. Config values that aren't sensible numbers fall back to defaults, the config
file must be a small regular file (no symlinks or pipes), and if no temperature sensor
can be read the curve runs the fans at full speed. `make test` checks the config parsing.

## How it works

Two pieces, mirroring how Macs Fan Control uses a privileged helper:

- **`fancurved`** — a small root daemon (installed via launchd) that performs
  the SMC writes. It reads its desired state from a config file and applies it
  every 2 seconds. Survives reboots.
- **FanCurve.app** — the SwiftUI menu-bar app. Runs as you (no root): reads the
  SMC directly for the live display, and writes the config file to tell the
  daemon what to do.

So you authorize once at install; the UI is friction-free after that.

```
config file  <--writes--  FanCurve.app (menu bar, your user)
     |
     +--reads-->  fancurved (root daemon)  --writes-->  SMC / fans
```

## Install (one command)

```bash
git clone https://github.com/noahberry01123/fancurve fancurve
cd fancurve
./install.sh
```

It builds the daemon and app, quits Macs Fan Control, installs everything
(asks for your password once), starts the daemon, and opens the app. Look for
the fan icon + temperature in your menu bar.

## Using it

Click the menu-bar icon:

- **Auto** — hand the fans back to macOS firmware control.
- **Manual** — a slider sets every fan to a % of its own min–max range.
- **Curve** — fans stay at minimum below the "start" temp, ramp linearly, and
  hit maximum at the "full speed" temp. Tracks the hottest CPU/GPU sensor.

Settings apply immediately and persist across reboots.

## Sleep and closed-lid behaviour

The daemon hands the fans back to Apple firmware whenever nobody is using the
machine, so a sleeping or shut MacBook stays quiet:

- **Asleep** — released just before the system goes down, so the fans stop
  instead of staying latched at your manual/curve speed.
- **Lid shut, no display up** (sleep, or a Power Nap dark wake) — released. If
  the machine gets genuinely hot (85 °C) the daemon takes control back.
- **Clamshell with an external display** — that's a machine in use, so your
  settings keep applying. It only lets go after 10 minutes with no input *and*
  a cool SoC.

"Released" means firmware auto, not fans-off, so macOS still cools the machine
in every one of these states. Transitions are logged one line each to
`/var/log/fancurve.log`. Desktop Macs have no lid, so they always stay under
your settings while awake.

## Uninstall

```bash
./uninstall.sh
```

Removes the daemon, app and config, and returns the fans to macOS control.

## Command-line tool (optional)

The repo also builds `fancurve`, a standalone CLI (no daemon needed):

```bash
make
./fancurve list
sudo ./fancurve set 0 4000      # force fan 0 (clamped to its max)
sudo ./fancurve curve 55 90 3   # temp curve in the foreground
sudo ./fancurve auto            # back to macOS control
```

## Files

| File | Purpose |
|------|---------|
| `smc.c` / `smc.h` | AppleSMC IOKit layer (read/write/enumerate) |
| `fanapi.c` / `fanapi.h` | Fan + temperature + config API (daemon & app) |
| `fancurved.c` | Root daemon |
| `gui/FanCurve.swift` | SwiftUI menu-bar app |
| `main.c` | Standalone CLI |
| `install.sh` / `uninstall.sh` | One-command setup / removal |
| `build_app.sh` | Compile the `.app` bundle |

## Requirements

Apple Silicon Mac, macOS 13+, Xcode command-line tools (`xcode-select --install`).
No third-party dependencies. Tested on M1 Pro / macOS 26.

## Related

- [Battery Tool](https://github.com/noahberry01123/battery-tool) — charge limit, heat guard and an
  Extreme Low Power Mode that runs everyday apps on the efficiency cores.

## License

MIT — see [LICENSE](LICENSE).

# synology-ch340-ups

A DSM 7 package that lets a Synology NAS monitor a Megatec/Q1 UPS connected
through a **CH340/CH341 USB-serial bridge**.

DSM only recognises USB-HID models, and its bundled NUT has no serial driver,
so such a UPS is invisible to it. This package adds NUT's `blazer_ser` driver
and the table entries DSM needs, then reapplies them after DSM updates.

## Compatibility

Any UPS speaking the Megatec/Q1 protocol over one of these bridges:

| USB ID | Chip |
|---|---|
| `1a86:7523` | CH340 |
| `1a86:5523` | CH341 |
| `1a86:7522` | CH340K / CH340E |
| `4348:5523` | CH341, older ID |

Tested on an **Ippon Back Basic 1050 Euro** with **DSM 7.2.1-69057** on a
**DS920+**.[^tested] Nothing is brand-specific — a port is accepted only after
the device answers the Megatec status query.

[^tested]: Only this combination has been verified on real hardware. Other
    Megatec UPSes behind the same bridges should work unchanged, and other
    DSM 7.x releases are likely fine but untested.

These bridges also carry Arduinos and plain USB-COM adapters, so to avoid
disturbing them the driver probes only ports whose USB vendor is QinHeng,
skips ports another program holds, and rejects anything that does not answer
the query. A non-UPS device on such a bridge costs one spurious "UPS
connected" line in DSM's log and nothing more.

## Build

Requires Docker. Everything else runs inside the container.

```sh
git clone --recurse-submodules https://github.com/<owner>/synology-ch340-ups.git
cd synology-ch340-ups

./build.sh                              # Linux, macOS, Git Bash on Windows
.\build.ps1                             # Windows PowerShell
```

The `.spk` lands in `./build`.

### Arguments

| Argument | Values | Default | Meaning |
|---|---|---|---|
| target arch | `x86_64`, `aarch64`, `armv7` | `x86_64` | toolchain to build with |
| `--spk-arch` / `-SpkArch` | any | per target arch | value written to `arch=` in `INFO` |

```sh
./build.sh aarch64
./build.sh aarch64 --spk-arch rtd1296
```

```powershell
.\build.ps1 aarch64
.\build.ps1 aarch64 -SpkArch rtd1296
```

`--spk-arch` decides which Synology models accept the package. `x86_64` covers
all Intel/AMD models. For ARM the default is a guess — check your model in
Synology's *Appendix A: Platform and Arch Value Mapping Table* and override it.

## Install

**Package Center → Manual Install**, pick the `.spk`, accept the third-party
warning. Then enable the UPS in **Control Panel → Hardware & Power → UPS**.

Uninstalling reverts every system change.

## Layout

```
build.sh, build.ps1    host entry points
Dockerfile, docker/    build environment and in-container build
external/nut           NUT submodule, pinned to v2.7.4
patches/               patches applied to that tag
helper/                setuid-root helper (C)
package/               INFO template, privilege, scripts, icons
tools/                 icon generator
```

## Notes

The helper is setuid root because DSM 7 leaves no alternative — `/dev/ttyUSB0`
is `0600 root:root`, and every lesser mechanism is rejected for third-party
packages:

| `conf/privilege` variant | DSM 7.2.1 |
|---|---|
| `defaults: run-as: root` | rejected |
| `ctrl-script` with `run-as: root` | rejected |
| `defaults: run-as: system` | rejected |
| no `conf/privilege` | rejected |
| `capabilities` (any value) | rejected |
| `tool` with `user: root`, `permission: 4750` | accepted |

The package directory is writable by the unprivileged package account, so the
helper trusts nothing from it: the driver is pinned by a build-time SHA-256,
hashed and copied through a single file descriptor, and no path comes from
`argv` or the environment.

Other limitations:

- `ups.mfr` shows zeros because the UPS firmware leaves that field blank.
  Model and firmware read correctly.
- `battery.charge` is estimated from voltage. Shutdown is unaffected: `upsmon`
  acts on the UPS's own `LB` flag.

## License

GPL-2.0-or-later, following NUT. The package ships a modified NUT build; the
patches in `patches/` are the corresponding source changes.

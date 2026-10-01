# ksystemstats-plugin-npu

An Intel NPU sensor plugin for **KDE Plasma System Monitor**.

It adds NPU sensors to [`ksystemstats`](https://invent.kde.org/plasma/ksystemstats), the daemon that
feeds `plasma-systemmonitor`. Once installed, NPU utilization, frequency and memory show up as
graphable sensors alongside CPU, GPU and memory.

Supports Intel Core Ultra processors with an integrated NPU (Meteor Lake and newer) running the
mainline `intel_vpu` kernel driver.

## Sensors

| Sensor | Unit | Description |
|---|---|---|
| Utilization | % | Share of time the NPU spent executing jobs |
| Frequency | MHz | Current NPU clock. Reports `0` while idle, since the clock is gated |
| Minimum Frequency | MHz | Lowest frequency the hardware supports |
| Efficient Frequency | MHz | Most power-efficient operating point |
| Maximum Frequency | MHz | Highest frequency the hardware supports |
| Memory | bytes | NPU memory currently in use |
| Scheduler Mode | `HW` / `OS` | Whether jobs are scheduled by the hardware or by the OS |
| Power State | `D0` / `D3hot` | `D0` while the NPU is active, `D3hot` when idle |

They appear in System Monitor under **NPU → Intel NPU 0**.

Observed on a Core Ultra 5 125H (Meteor Lake) — idle versus under load:

```
idle                          under load
powerState = D3hot            powerState = D0
frequency  = 0                frequency  = 700
busy       = 0                busy       = 9.55
memory     = 68722688         memory     = 81399808

frequencyMin       = 333      (constant)
frequencyEfficient = 700      (constant)
frequencyMax       = 1400     (constant)
schedulerMode      = HW       (constant)
```

Sensors whose backing attribute is missing are skipped, so the plugin degrades gracefully on
kernels that do not expose all of them.

## Requirements

* Linux with the `intel_vpu` kernel module loaded (`lsmod | grep intel_vpu`)
* `/dev/accel/accel0` present — check with `ls /sys/class/accel/`
* KDE Plasma 6 with `ksystemstats`

On Arch Linux:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base kcoreaddons ki18n libksysguard
```

## Build

```bash
git clone https://github.com/Galaxy1108/ksystemstats-plugin-npu.git
cd ksystemstats-plugin-npu
cmake -S . -B build -G Ninja
cmake --build build
```

The result is `build/ksystemstats_plugin_npu.so`.

## Install

```bash
sudo install -Dm755 build/ksystemstats_plugin_npu.so \
    /usr/lib/qt6/plugins/ksystemstats/ksystemstats_plugin_npu.so

systemctl --user restart plasma-ksystemstats.service
```

Open **System Monitor**, create a new page, and add the sensors from the **NPU** section.

## Uninstall

```bash
sudo rm /usr/lib/qt6/plugins/ksystemstats/ksystemstats_plugin_npu.so
systemctl --user restart plasma-ksystemstats.service
```

## Command-line monitor

The repository also includes `npu-top.sh`, a standalone monitor for the same sensors that needs
neither root nor any dependencies:

```bash
./npu-top.sh          # refresh every second
./npu-top.sh 0.5      # refresh every 0.5 s
```

## How it works

The plugin reads the NPU's sysfs attributes directly from
`/sys/class/accel/accel0/device/`, all of which are world-readable, so no elevated privileges are
required.

Utilization is derived from `npu_busy_time_us`, a cumulative counter of the time the NPU spent
executing jobs. The kernel documentation recommends sampling it no more than once per second to
avoid affecting job submission, so the sensor throttles its reads accordingly.

## Testing

`loadtest` verifies the plugin against the same discovery path the daemon uses, without installing
anything:

```bash
mkdir -p /tmp/npuplugintest/ksystemstats
cp build/ksystemstats_plugin_npu.so /tmp/npuplugintest/ksystemstats/

QT_PLUGIN_PATH=/tmp/npuplugintest QT_FORCE_STDERR_LOGGING=1 ./build/loadtest
```

## License

LGPL-2.0-or-later

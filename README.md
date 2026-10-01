# ksystemstats-plugin-npu

Intel NPU (`intel_vpu`) sensor plugin for **KDE Plasma System Monitor** (`plasma-systemmonitor`).

Adds NPU utilization, frequency and memory sensors to `ksystemstats` — the sensor daemon behind
Plasma's System Monitor. KDE upstream ships no NPU/`accel` plugin, and neither of its two generic
paths can see the NPU:

* the `lmsensors` plugin reads `/sys/class/hwmon/*` — `intel_vpu` registers **no** hwmon device
* the `gpu` plugin enumerates `/sys/class/drm/card*` — the NPU lives in the separate **`accel`**
  subsystem (`/dev/accel/accel0`, major 261), not a DRM card node

So this plugin reads the NPU's sysfs attributes directly.

> 面向中文用户的完整说明、踩坑记录和替代方案见下文。

---

## Sensors

### Exposed by this plugin

| Sensor id | Source (`/sys/class/accel/accel0/device/`) | Unit | Notes |
|---|---|---|---|
| `busy` | `npu_busy_time_us` | % | Computed from the delta of the cumulative busy counter |
| `frequency` | `npu_current_frequency_mhz` | MHz | Returns `0` when idle (the clock is gated) |
| `memory` | `npu_memory_utilization` | bytes | Sum of resident NPU buffers |

Sensor paths in System Monitor: `npu/npu0/busy`, `npu/npu0/frequency`, `npu/npu0/memory`.

### Also available from the NPU, not yet exposed

Easy to add if you want them:

| Source | Meaning | Observed value on Meteor Lake |
|---|---|---|
| `sched_mode` | Scheduling mode (`HW` / `OS`) | `HW` |
| `power_state` | `D3hot` = idle, `D0` = active | `D3hot` |
| `freq/hw_min_freq` | Minimum hardware frequency | 333 MHz |
| `freq/hw_efficient_freq` | Most efficient frequency | 700 MHz |
| `freq/hw_max_freq` | Maximum hardware frequency | 1400 MHz |
| `freq/set_min_freq`, `freq/set_max_freq` | Writable frequency limits | **Not present** — 50XX (Panther Lake) and newer only |

### Not available via sysfs (needs PMT, root)

Power (W), temperature (°C), DDR bandwidth and tile configuration are only reachable through
**Intel PMT** (`/sys/class/intel_pmt/telem*`). That is what Intel's
[`npu-monitor-tool`](https://github.com/open-edge-platform/edge-ai-libraries/tree/main/tools/npu-monitor-tool)
uses. This plugin deliberately stays unprivileged and dependency-free.

---

## Verified output

Measured on Arch Linux + KDE 6.7.5 + Core Ultra 5 125H, while running `intel-npu-umd-test` to
generate real NPU load:

```
container npu / NPU
  object npu0 / Intel NPU 0
    sensor busy       Utilization  unit=1002  path=npu/npu0/busy
    sensor memory     Memory       unit=100   path=npu/npu0/memory
    sensor frequency  Frequency    unit=302   path=npu/npu0/frequency

#0 memory     = 102285312
#0 frequency  = 0
#0 busy       = 0
#1 memory     = 102281216
#1 frequency  = 0
#1 busy       = 9.880454545454546     <- 9.88% utilization
#2 memory     = 68726784
#2 frequency  = 700                   <- 0 when idle, ramps up under load
#2 busy       = 9.482090909090909
```

## Kernel behaviour that shaped the design

From `drivers/accel/ivpu/ivpu_sysfs.c`:

> npu_busy_time_us is the time that the device spent executing jobs. ...
> This time can be used to measure the utilization of NPU, either by calculating
> npu_busy_time_us difference between two timepoints ... or monitoring utilization
> percentage by reading npu_busy_time_us periodically.
>
> When reading the value periodically, it shouldn't be read too often as it may have
> an impact on job submission performance. **Recommended period is 1 second.**

`ksystemstats` refreshes at 500 ms (`KSysGuard::BackendUpdateInterval`), which is faster than the
kernel recommends, so `NpuUtilizationSensor` throttles its own reads to 1 second.

---

## Build

Dependencies — all available on Arch, and **`extra-cmake-modules` is not required**:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base kcoreaddons ki18n libksysguard
```

```bash
git clone https://github.com/Galaxy1108/ksystemstats-plugin-npu.git
cd ksystemstats-plugin-npu
cmake -S . -B build -G Ninja
cmake --build build
```

Output: `build/ksystemstats_plugin_npu.so`

## Install

```bash
sudo install -Dm755 build/ksystemstats_plugin_npu.so \
    /usr/lib/qt6/plugins/ksystemstats/ksystemstats_plugin_npu.so

systemctl --user restart plasma-ksystemstats.service
```

Then open **System Monitor** → *New page* → drag in sensors, and look under `npu / npu0`
for **Utilization**, **Frequency** and **Memory**.

To uninstall, just remove the `.so` and restart the service:

```bash
sudo rm /usr/lib/qt6/plugins/ksystemstats/ksystemstats_plugin_npu.so
systemctl --user restart plasma-ksystemstats.service
```

> The filename must be exactly `ksystemstats_plugin_npu.so` — **no `lib` prefix**. That is the
> convention expected by `KPluginMetaData::findPlugins("ksystemstats")`. The CMake target sets
> `PREFIX ""` to guarantee it.

## Verify without installing

`loadtest` replicates the daemon's plugin discovery path (KDE `ksystemstats/src/daemon.cpp:95`)
against a throwaway plugin tree:

```bash
mkdir -p /tmp/npuplugintest/ksystemstats
cp build/ksystemstats_plugin_npu.so /tmp/npuplugintest/ksystemstats/

QT_PLUGIN_PATH=/tmp/npuplugintest QT_FORCE_STDERR_LOGGING=1 ./build/loadtest
```

`QT_FORCE_STDERR_LOGGING=1` is required: when stderr is not a TTY, Qt6 redirects logging to the
systemd journal and you will see no output at all.

`npu-top.sh` in this repo is a standalone, unprivileged CLI monitor for the same sensors:

```bash
./npu-top.sh          # 1 s refresh
./npu-top.sh 0.5      # 0.5 s refresh
```

---

## Gotchas hit while writing this

1. **`SensorProperty` / `SysFsSensor` auto-register with their parent `SensorObject`.**
   Passing a `SensorObject*` to the constructor is enough. Calling `object->addProperty(...)`
   afterwards triggers
   `Add property "x" to object "y" that already contains a property with that ID, overwriting`.
   KDE's own plugins (e.g. `GpuDevice.cpp`) never call `addProperty`.

2. **`SensorObject` does *not* auto-add itself to a `SensorContainer`.**
   Call `container->addObject(object)` explicitly.

3. **`SysFsSensor` only reads sysfs once the sensor is subscribed.**
   With no subscriber, `value()` stays invalid/empty. `plasma-systemmonitor` subscribes
   automatically, but a hand-written test must call `sensor->subscribe()` first.

4. **`SensorPlugin` lives in `KSysGuard::SystemStats`, not `KSysGuard::Sensors`.**
   Linking the wrong one still compiles, because Linux `MODULE` libraries allow undefined symbols
   by default — but it is incorrect. `KSysGuard::Sensors` is the Plasma widget library.

5. **`SensorInfo.h` includes `QDBusArgument`**, so `Qt6::DBus` must be linked.

6. **`find_package(KF6 COMPONENTS ...)` fails without ECM.** Use the concrete package names
   `KF6CoreAddons` / `KF6I18n` instead.

7. **Qt6 redirects logging to systemd journal** when stderr is not a TTY. Set
   `QT_FORCE_STDERR_LOGGING=1` when debugging.

---

## Alternatives

| Option | Platform | Capabilities | Notes |
|---|---|---|---|
| **GNOME Resources** | any (GTK app) | Graphical, includes NPU utilization | NPU support since v1.7. Runs fine under Plasma. `flatpak install flathub net.nokyan.Resources` |
| **Intel `npu-monitor-tool`** | CLI | Power (W), temperature (°C), DDR bandwidth, tile config, utilization, memory | Uses PMT, needs root |
| **`npu-top.sh`** (this repo) | CLI | Utilization, frequency, memory, power state | No deps, no root |
| **This plugin** | Plasma integration | Utilization, frequency, memory | Graphs and alerts inside System Monitor |

## License

LGPL-2.0-or-later — matching the KDE Frameworks plugin ecosystem.

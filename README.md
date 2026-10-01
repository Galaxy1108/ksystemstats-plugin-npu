# ksystemstats-plugin-npu

An Intel NPU sensor plugin for **KDE Plasma System Monitor**.

It adds NPU sensors to [`ksystemstats`](https://invent.kde.org/plasma/ksystemstats), the daemon that
feeds `plasma-systemmonitor`. Once installed, NPU utilization, frequency, memory, temperature and
power show up as graphable sensors alongside CPU, GPU and memory.

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
| Temperature ¹ | °C | NPU temperature, read through Intel PMT telemetry |
| Power ¹ | W | NPU power draw, derived from the PMT energy counter |
| Voltage ¹ | raw | Raw PMT voltage field. No public conversion to volts is documented |

¹ Requires a one-time permission change — see
[Enabling power and temperature](#enabling-power-and-temperature). Without it these three sensors
are simply absent; everything else works out of the box.

They appear in System Monitor under **NPU → Intel NPU 0**.

Observed on a Core Ultra 5 125H (Meteor Lake) — idle versus under load:

```
idle                          under load
powerState  = D3hot           powerState  = D0
frequency   = 0               frequency   = 700
busy        = 0               busy        = 9.34
memory      = 68722688        memory      = 68743168
temperature = 50              temperature = 50
voltage     = 5               voltage     = 22
power       = 0               power       = 0.356

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

## Enabling power and temperature

Temperature, power and voltage come from Intel's **PMT** (Platform Monitoring Technology) rather
than from hwmon, because the `intel_vpu` driver does not register a hwmon device at all. The kernel
creates the telemetry attribute as `0440 root:root`, so by default only root can read it.

Since the attribute is *group*-readable (`0440`, not `0400`), granting access needs no root, no
helper binary and no capability — just a different owning group:

```bash
# 1. Create a dedicated group and join it.
#    Log out and back in afterwards so the new membership takes effect.
sudo groupadd -r intelpmt
sudo usermod -aG intelpmt $USER

# 2. Have udev re-own the NPU telemetry block, now and on every boot.
sudo tee /etc/udev/rules.d/70-intel-pmt-npu.rules >/dev/null <<'EOF'
SUBSYSTEM=="intel_pmt", ATTR{guid}=="0x130670b2", RUN+="/bin/chgrp intelpmt /sys%p/telem"
EOF

# 3. Apply it.
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=intel_pmt

# 4. Verify — expect "root intelpmt" and a readable file.
ls -l /sys/class/intel_pmt/telem1/telem
head -c 16 /sys/class/intel_pmt/telem1/telem | xxd
```

Then restart the sensor daemon:

```bash
systemctl --user restart plasma-ksystemstats.service
```

Notes:

* The rule matches on the NPU's PMT **GUID**, not on the block name, because `telemN` numbering is
  not stable across platforms. `0x130670b2` is the Meteor Lake NPU GUID; the other platforms this
  plugin knows about are listed in `NpuPmtReader::registerMapForGuid()` in `NpuPlugin.cpp`.
* Other `telemN` blocks belong to different agents and stay root-only.
* To undo, remove the rule and the group and restart the daemon:

  ```bash
  sudo rm /etc/udev/rules.d/70-intel-pmt-npu.rules
  sudo groupdel intelpmt
  systemctl --user restart plasma-ksystemstats.service
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

The repository also includes `npu-top.sh`, a standalone monitor for the sysfs-based sensors that
needs neither root nor any dependencies:

```bash
./npu-top.sh          # refresh every second
./npu-top.sh 0.5      # refresh every 0.5 s
```

## How it works

Utilization, frequency, memory, scheduler mode and power state are read straight from the
world-readable attributes under `/sys/class/accel/accel0/device/`, so no privileges are involved.

Utilization is derived from `npu_busy_time_us`, a cumulative counter of the time the NPU spent
executing jobs. The kernel documentation recommends sampling it no more than once per second to
avoid affecting job submission, so the sensor throttles its reads accordingly.

Temperature and power require PMT (see above). That telemetry is a raw register snapshot, so it has
to be decoded by bit field: temperature is bits 47..40 of `SOC_TEMPERATURES`, and the energy counter
`VPU_ENERGY` is a U32.18.14 fixed-point value in joules, from which power is computed as
Δenergy / Δtime. Register offsets differ per platform and are selected by the PMT block's GUID.

## Testing

`loadtest` verifies the plugin against the same discovery path the daemon uses, without installing
anything:

```bash
mkdir -p /tmp/npuplugintest/ksystemstats
cp build/ksystemstats_plugin_npu.so /tmp/npuplugintest/ksystemstats/

QT_PLUGIN_PATH=/tmp/npuplugintest QT_FORCE_STDERR_LOGGING=1 ./build/loadtest
```

To test the PMT sensors without logging out first, run it with the group attached:

```bash
sudo setpriv --reuid=$(id -u) --regid=$(id -g) --groups=$(getent group intelpmt | cut -d: -f3) \
    --inh-caps=-all -- env QT_PLUGIN_PATH=/tmp/npuplugintest \
    QT_FORCE_STDERR_LOGGING=1 ./build/loadtest
```

## Translations

The plugin uses its own translation domain, `ksystemstats_plugin_npu`, rather than KDE's shared
`ksystemstats_plugins` catalogue. That is deliberate: KDE translates `"Power"` as 电源 (power
supply), which is wrong for the wattage sensor here.

Chinese (Simplified) is included in `po/zh_CN.po` and installed automatically:

| Sensor | 中文 |
|---|---|
| Utilization | 利用率 |
| Frequency | 频率 |
| Minimum / Efficient / Maximum Frequency | 最低频率 / 能效频率 / 最高频率 |
| Memory | 内存 |
| Scheduler Mode | 调度模式 |
| Power State | 电源状态 |
| Temperature | 温度 |
| Power | 功耗 |
| Voltage (raw) | 电压（原始值） |

Sensor names are looked up through `i18ndc()` with the domain passed explicitly, so loading this
plugin never changes the translation domain of the process and cannot affect KDE's own plugins.

Adding another language:

```bash
cp po/ksystemstats_plugin_npu.pot po/<lang>.po
# ... 翻译后校验
msgfmt --check po/<lang>.po
```

Every `po/*.po` is compiled during the build and installed to
`/usr/share/locale/<lang>/LC_MESSAGES/ksystemstats_plugin_npu.mo`.

Regenerating the template after changing sensor strings:

```bash
xgettext --language=C++ --keyword=i18ndc:2c,3 --keyword=i18ndc:2c,3,4 \
    --from-code=UTF-8 -o po/ksystemstats_plugin_npu.pot NpuPlugin.cpp
```

## License

LGPL-2.0-or-later

/*
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "NpuPlugin.h"

#include <KPluginFactory>
#include <KLocalizedString>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QStringList>
#include <QVariant>

#include <functional>
#include <memory>

#include <systemstats/SensorContainer.h>
#include <systemstats/SensorObject.h>
#include <systemstats/SensorProperty.h>
#include <systemstats/SysFsSensor.h>

namespace
{
constexpr auto AccelRoot = "/sys/class/accel";
constexpr auto PmtRoot = "/sys/class/intel_pmt";

/**
 * 返回第一个存在的路径，都不存在时返回空。
 * 部分属性在内核版本之间改过名字，这里做兼容。
 */
QString firstExisting(const QStringList &candidates)
{
    for (const auto &path : candidates) {
        if (QFile::exists(path)) {
            return path;
        }
    }
    return {};
}

/**
 * 只有属性文件确实存在时才创建传感器，这样在不支持某些属性的平台上可以优雅降级。
 */
KSysGuard::SysFsSensor *addNumericSensor(KSysGuard::SensorObject *object,
                                         const QString &id,
                                         const QString &name,
                                         const QString &path,
                                         KSysGuard::Unit unit)
{
    if (path.isEmpty() || !QFile::exists(path)) {
        return nullptr;
    }

    auto *sensor = new KSysGuard::SysFsSensor(id, path, object);
    sensor->setName(name);
    sensor->setShortName(i18nc("@title", "NPU"));
    sensor->setUnit(unit);
    return sensor;
}

/**
 * 字符串型传感器，用于 sched_mode、power_state 这类非数值属性。
 */
KSysGuard::SysFsSensor *addStringSensor(KSysGuard::SensorObject *object,
                                        const QString &id,
                                        const QString &name,
                                        const QString &path)
{
    auto *sensor = addNumericSensor(object, id, name, path, KSysGuard::UnitNone);
    if (sensor) {
        sensor->setVariantType(QVariant::String);
        sensor->setConvertFunction([](const QByteArray &data) {
            return QVariant(QString::fromLatin1(data.trimmed()));
        });
    }
    return sensor;
}

/**
 * 从 buffer 的 offset 处取小端 8 字节，再抽出 [msb:lsb] 位域。
 */
quint64 readField(const QByteArray &buffer, int offset, int msb, int lsb)
{
    if (offset < 0 || offset + 8 > buffer.size() || msb < lsb || lsb < 0) {
        return 0;
    }

    quint64 data = 0;
    for (int i = 0; i < 8; ++i) {
        data |= quint64(quint8(buffer.at(offset + i))) << (8 * i);
    }

    const quint64 msbMask = (msb >= 63) ? ~quint64(0) : ((quint64(1) << (msb + 1)) - 1);
    const quint64 lsbMask = (lsb > 0) ? ((quint64(1) << lsb) - 1) : quint64(0);
    return (data & msbMask & ~lsbMask) >> lsb;
}
}

/**
 * NPU 的功耗和温度只能从 Intel PMT（Platform Monitoring Technology）遥测块读取，
 * 驱动的 hwmon 里没有这些。数据是一整块原始寄存器快照，需要按位域解析。
 *
 * 寄存器布局来自 Intel 官方的 npu-monitor-tool：
 *   Meteor Lake / Arrow Lake   VPU_ENERGY 0x628, SOC_TEMPERATURES 0x98, VPU_WORKPOINT 0x68
 *   Lunar Lake                 VPU_ENERGY 0x5d0, SOC_TEMPERATURES 0x70, VPU_WORKPOINT 0x18
 *   Panther Lake / Wildcat Lake VPU_ENERGY 0x670, SOC_TEMPERATURES 0x78, VPU_WORKPOINT 0x18
 *
 * 温度 = SOC_TEMPERATURES 的 bit 47..40，单位摄氏度
 * 电压 = VPU_WORKPOINT 的 bit 15..8（原始值，没有公开的换算关系）
 * 功耗 = VPU_ENERGY 的差分。它是 U32.18.14 定点累计焦耳，
 *        所以 功耗(W) = Δ能量(J) / Δ时间(s)
 *
 * telem 属性的权限被内核写死为 0440 root:root，普通用户默认读不了。
 * 读不到时这个类不会被创建，插件会自动跳过这几项传感器 —— 见 README 的授权说明。
 */
class NpuPmtReader
{
public:
    struct RegisterMap {
        int energy;
        int socTemperatures;
        int vpuWorkpoint;
    };

    static std::shared_ptr<NpuPmtReader> create()
    {
        const QDir pmtDir(QString::fromLatin1(PmtRoot));
        if (!pmtDir.exists()) {
            return {};
        }

        const auto blocks = pmtDir.entryList({QStringLiteral("telem*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

        for (const auto &block : blocks) {
            const QString blockPath = pmtDir.filePath(block);

            QFile guidFile(blockPath + QStringLiteral("/guid"));
            if (!guidFile.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QString guid = QString::fromLatin1(guidFile.readAll().trimmed());

            const RegisterMap *map = registerMapForGuid(guid);
            if (!map) {
                continue;
            }

            const QString telemPath = blockPath + QStringLiteral("/telem");

            // 权限没配好时直接放弃，不要留下永远读不出值的传感器。
            QFile probe(telemPath);
            if (!probe.open(QIODevice::ReadOnly)) {
                return {};
            }
            probe.close();

            auto reader = std::shared_ptr<NpuPmtReader>(new NpuPmtReader(telemPath, *map));
            reader->refresh();
            return reader;
        }

        return {};
    }

    void refresh()
    {
        // 和利用率一样限制采样频率：官方工具的默认间隔也是 1000ms。
        if (m_lastRead.isValid() && m_lastRead.elapsed() < 1000) {
            return;
        }

        QFile file(m_path);
        if (!file.open(QIODevice::ReadOnly)) {
            return;
        }
        const QByteArray buffer = file.readAll();
        if (buffer.isEmpty()) {
            return;
        }

        const qreal elapsedSeconds = m_lastRead.isValid() ? (m_lastRead.elapsed() / 1000.0) : 0.0;
        m_lastRead.restart();
        m_buffer = buffer;

        m_temperature = qreal(readField(m_buffer, m_map.socTemperatures, 47, 40));
        m_voltage = qreal(readField(m_buffer, m_map.vpuWorkpoint, 15, 8));

        const qreal energy = energyJoules();
        if (elapsedSeconds > 0.0) {
            m_power = (energy - m_lastEnergy) / elapsedSeconds;
        }
        m_lastEnergy = energy;
    }

    qreal temperature() const
    {
        return m_temperature;
    }

    qreal power() const
    {
        return m_power;
    }

    qreal voltage() const
    {
        return m_voltage;
    }

private:
    NpuPmtReader(QString path, const RegisterMap &map)
        : m_path(std::move(path))
        , m_map(map)
    {
    }

    // U32.18.14 定点数 -> 焦耳
    qreal energyJoules() const
    {
        const quint64 raw = readField(m_buffer, m_map.energy, 63, 0);
        return qreal(raw >> 14) + qreal(raw & ((quint64(1) << 14) - 1)) / 16384.0;
    }

    static const RegisterMap *registerMapForGuid(const QString &guid)
    {
        // GUID 见 Intel npu-monitor-tool 的 PMT_GUID_* 常量。
        static const RegisterMap mtl = {0x628, 0x98, 0x68};  // Meteor Lake, Arrow Lake
        static const RegisterMap lnl = {0x5d0, 0x70, 0x18};  // Lunar Lake
        static const RegisterMap ptl = {0x670, 0x78, 0x18};  // Panther Lake, Wildcat Lake

        static const QHash<QString, const RegisterMap *> maps = {
            {QStringLiteral("0x130670b2"), &mtl},  // Meteor Lake
            {QStringLiteral("0x1306a0b2"), &mtl},  // Arrow Lake-H
            {QStringLiteral("0x1306a0b3"), &mtl},  // Arrow Lake
            {QStringLiteral("0x1306a0b4"), &mtl},  // Arrow Lake-S
            {QStringLiteral("0x3072005"), &lnl},   // Lunar Lake
            {QStringLiteral("0x3086000"), &ptl},   // Panther Lake
            {QStringLiteral("0x308d000"), &ptl},   // Wildcat Lake
        };

        return maps.value(guid, nullptr);
    }

    QString m_path;
    RegisterMap m_map;
    QByteArray m_buffer;
    QElapsedTimer m_lastRead;
    qreal m_lastEnergy = 0.0;
    qreal m_temperature = 0.0;
    qreal m_power = 0.0;
    qreal m_voltage = 0.0;
};

/**
 * 把 NpuPmtReader 的某个字段暴露成传感器。
 * 实际读取由 NpuPlugin::update() 统一触发，多个传感器共享同一份缓冲。
 */
class NpuPmtSensor : public KSysGuard::SensorProperty
{
    Q_OBJECT
public:
    using Getter = std::function<qreal()>;

    NpuPmtSensor(const QString &id,
                 const QString &name,
                 KSysGuard::Unit unit,
                 const QString &description,
                 std::shared_ptr<NpuPmtReader> reader,
                 Getter getter,
                 KSysGuard::SensorObject *parent)
        : SensorProperty(id, name, parent)
        , m_reader(std::move(reader))
        , m_getter(std::move(getter))
    {
        setShortName(i18nc("@title", "NPU"));
        setUnit(unit);
        setDescription(description);
    }

    void update() override
    {
        setValue(m_getter());
    }

private:
    std::shared_ptr<NpuPmtReader> m_reader;
    Getter m_getter;
};

/**
 * NPU 利用率传感器。
 *
 * 内核文档（drivers/accel/ivpu/ivpu_sysfs.c）对 npu_busy_time_us 的说明是：
 *   它是设备执行任务的累计时间，需要取两次采样的差值再除以墙钟时间
 *   才能得到利用率百分比。
 * 并且明确建议："When reading the value periodically, it shouldn't be read
 *   too often as it may have an impact on job submission performance.
 *   Recommended period is 1 second."
 *
 * ksystemstats 的后端刷新间隔是 500ms（KSysGuard::BackendUpdateInterval），
 * 比内核建议更快，所以这里自行节流到 1 秒。
 */
class NpuUtilizationSensor : public KSysGuard::SensorProperty
{
    Q_OBJECT
public:
    NpuUtilizationSensor(const QString &path, KSysGuard::SensorObject *parent)
        : SensorProperty(QStringLiteral("busy"), i18nc("@title", "Utilization"), parent)
        , m_path(path)
    {
        setShortName(i18nc("@title", "NPU"));
        setUnit(KSysGuard::UnitPercent);
        setMin(0);
        setMax(100);
        setDescription(i18nc("@info", "Time the NPU spent executing jobs, as a percentage of the sampling interval."));
        m_timer.start();
    }

    void update() override
    {
        // 遵守内核建议：采样周期不短于 1 秒。
        // 但第一次采样必须放行，否则在拿到基线之前传感器会一直是空的。
        const bool firstSample = (m_lastBusyUs < 0);
        if (!firstSample && m_timer.elapsed() < 1000) {
            return;
        }

        QFile file(m_path);
        if (!file.open(QIODevice::ReadOnly)) {
            return;
        }

        bool ok = false;
        const qint64 busyUs = file.readAll().trimmed().toLongLong(&ok);
        if (!ok) {
            return;
        }

        if (firstSample) {
            // 第一次只建立基线，还没有可比较的区间。
            m_lastBusyUs = busyUs;
            m_timer.restart();
            setValue(0.0);
            return;
        }

        const qint64 elapsedUs = m_timer.restart() * 1000;
        const qint64 delta = busyUs - m_lastBusyUs;
        m_lastBusyUs = busyUs;

        const qreal percent = elapsedUs > 0 ? (100.0 * double(delta) / double(elapsedUs)) : 0.0;
        setValue(qBound(qreal(0), percent, qreal(100)));
    }

private:
    QString m_path;
    QElapsedTimer m_timer;
    qint64 m_lastBusyUs = -1;
};

class NpuPlugin::Private
{
public:
    std::unique_ptr<KSysGuard::SensorContainer> container;
    QList<std::shared_ptr<NpuPmtReader>> pmtReaders;
};

NpuPlugin::NpuPlugin(QObject *parent, const QVariantList &args)
    : SensorPlugin(parent, args)
    , d(std::make_unique<Private>())
{
    // 插件在 ksystemstats 加载时就要构造出带名字的传感器，
    // 这里确保翻译域已就绪，否则每个 i18nc 都会打一条 "Domain is not set" 警告。
    KLocalizedString::setApplicationDomain("ksystemstats");

    d->container = std::make_unique<KSysGuard::SensorContainer>(QStringLiteral("npu"), i18nc("@title", "NPU"), this);

    const QDir accelDir(QString::fromLatin1(AccelRoot));
    if (!accelDir.exists()) {
        return;
    }

    const auto devices = accelDir.entryList({QStringLiteral("accel*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    int index = 0;
    for (const auto &deviceName : devices) {
        const QString base = accelDir.filePath(deviceName) + QStringLiteral("/device");

        // 只认 intel_vpu 驱动的设备，避免把其它 accel 设备误报成 NPU。
        QFile uevent(base + QStringLiteral("/uevent"));
        if (!uevent.open(QIODevice::ReadOnly) || !uevent.readAll().contains("DRIVER=intel_vpu")) {
            continue;
        }

        auto *object = new KSysGuard::SensorObject(QStringLiteral("npu%1").arg(index),
                                                   i18nc("@title", "Intel NPU %1", index));
        d->container->addObject(object);

        // 注意：SensorProperty / SysFsSensor 的构造函数在传入 SensorObject 作为 parent 时
        // 会自动把自己注册到该对象上，所以这里不能再调用 addProperty()。
        new NpuUtilizationSensor(base + QStringLiteral("/npu_busy_time_us"), object);

        addNumericSensor(object, QStringLiteral("frequency"), i18nc("@title", "Frequency"),
                         firstExisting({base + QStringLiteral("/freq/current_freq"),
                                        base + QStringLiteral("/npu_current_frequency_mhz")}),
                         KSysGuard::UnitMegaHertz);

        addNumericSensor(object, QStringLiteral("frequencyMin"), i18nc("@title", "Minimum Frequency"),
                         base + QStringLiteral("/freq/hw_min_freq"),
                         KSysGuard::UnitMegaHertz);

        addNumericSensor(object, QStringLiteral("frequencyEfficient"), i18nc("@title", "Efficient Frequency"),
                         base + QStringLiteral("/freq/hw_efficient_freq"),
                         KSysGuard::UnitMegaHertz);

        addNumericSensor(object, QStringLiteral("frequencyMax"), i18nc("@title", "Maximum Frequency"),
                         firstExisting({base + QStringLiteral("/freq/hw_max_freq"),
                                        base + QStringLiteral("/npu_max_frequency_mhz")}),
                         KSysGuard::UnitMegaHertz);

        addNumericSensor(object, QStringLiteral("memory"), i18nc("@title", "Memory"),
                         base + QStringLiteral("/npu_memory_utilization"),
                         KSysGuard::UnitByte);

        addStringSensor(object, QStringLiteral("schedulerMode"), i18nc("@title", "Scheduler Mode"),
                        base + QStringLiteral("/sched_mode"));

        addStringSensor(object, QStringLiteral("powerState"), i18nc("@title", "Power State"),
                        base + QStringLiteral("/power_state"));

        // 功耗和温度走 PMT。读不到就整个跳过 —— 没有配置授权时插件照常工作。
        if (auto reader = NpuPmtReader::create()) {
            d->pmtReaders.append(reader);

            new NpuPmtSensor(QStringLiteral("temperature"), i18nc("@title", "Temperature"),
                             KSysGuard::UnitCelsius,
                             i18nc("@info", "NPU temperature reported through Intel PMT telemetry."),
                             reader, [reader] { return reader->temperature(); }, object);

            new NpuPmtSensor(QStringLiteral("power"), i18nc("@title", "Power"),
                             KSysGuard::UnitWatt,
                             i18nc("@info", "NPU power consumption, derived from the PMT energy counter."),
                             reader, [reader] { return reader->power(); }, object);

            new NpuPmtSensor(QStringLiteral("voltage"), i18nc("@title", "Voltage (raw)"),
                             KSysGuard::UnitNone,
                             i18nc("@info", "Raw PMT voltage field. No public conversion to volts is documented."),
                             reader, [reader] { return reader->voltage(); }, object);
        }

        ++index;
    }
}

NpuPlugin::~NpuPlugin() = default;

void NpuPlugin::update()
{
    // PMT 是整块快照，先统一刷新一次，三个传感器共享同一份缓冲。
    for (const auto &reader : d->pmtReaders) {
        reader->refresh();
    }

    const auto containers = this->containers();
    for (auto *container : containers) {
        const auto objects = container->objects();
        for (auto *object : objects) {
            const auto sensors = object->sensors();
            for (auto *sensor : sensors) {
                sensor->update();
            }
        }
    }
}

K_PLUGIN_CLASS_WITH_JSON(NpuPlugin, "metadata.json")

#include "NpuPlugin.moc"

/*
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "NpuPlugin.h"

#include <KPluginFactory>
#include <KLocalizedString>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>

#include <systemstats/SensorContainer.h>
#include <systemstats/SensorObject.h>
#include <systemstats/SensorProperty.h>
#include <systemstats/SysFsSensor.h>

namespace
{
constexpr auto AccelRoot = "/sys/class/accel";
}

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
};

NpuPlugin::NpuPlugin(QObject *parent, const QVariantList &args)
    : SensorPlugin(parent, args)
    , d(std::make_unique<Private>())
{
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
        // 会自动把自己注册到该对象上，所以这里不能再调用 addProperty()，
        // 否则会触发 "already contains a property with that ID, overwriting" 警告。
        new NpuUtilizationSensor(base + QStringLiteral("/npu_busy_time_us"), object);

        auto *frequency = new KSysGuard::SysFsSensor(QStringLiteral("frequency"),
                                                     base + QStringLiteral("/npu_current_frequency_mhz"),
                                                     object);
        frequency->setName(i18nc("@title", "Frequency"));
        frequency->setShortName(i18nc("@title", "NPU"));
        frequency->setUnit(KSysGuard::UnitMegaHertz);

        auto *memory = new KSysGuard::SysFsSensor(QStringLiteral("memory"),
                                                  base + QStringLiteral("/npu_memory_utilization"),
                                                  object);
        memory->setName(i18nc("@title", "Memory"));
        memory->setShortName(i18nc("@title", "NPU"));
        memory->setUnit(KSysGuard::UnitByte);

        ++index;
    }
}

NpuPlugin::~NpuPlugin() = default;

void NpuPlugin::update()
{
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

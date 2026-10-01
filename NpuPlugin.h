/*
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#pragma once

#include <memory>

#include <systemstats/SensorPlugin.h>

/**
 * Intel NPU (intel_vpu) 传感器插件。
 *
 * 数据来源全部是内核 ivpu 驱动暴露的 sysfs 属性，
 * 位于 /sys/class/accel/accel<N>/device/ 。
 */
class NpuPlugin : public KSysGuard::SensorPlugin
{
    Q_OBJECT
public:
    NpuPlugin(QObject *parent, const QVariantList &args);
    ~NpuPlugin() override;

    QString providerName() const override
    {
        return QStringLiteral("npu");
    }

    void update() override;

private:
    class Private;
    std::unique_ptr<Private> d;
};

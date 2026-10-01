// 完全复刻 ksystemstats 守护进程的插件发现流程（见 KDE/ksystemstats src/daemon.cpp:95），
// 用来在不安装的前提下验证插件是否真的能被加载。
//
// 用法:
//   QT_PLUGIN_PATH=/tmp/npuplugintest ./loadtest
// 其中 /tmp/npuplugintest/ksystemstats/ksystemstats_plugin_npu.so 是插件

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QThread>
#include <cstdio>

#include <KPluginFactory>
#include <KPluginMetaData>

#include <systemstats/SensorContainer.h>
#include <systemstats/SensorObject.h>
#include <systemstats/SensorPlugin.h>
#include <systemstats/SensorProperty.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    fprintf(stderr, "\n[1] 搜索路径:\n");
    for (const auto &p : QCoreApplication::libraryPaths()) {
        fprintf(stderr, "    %s\n", qPrintable(p));
    }

    // 这一行与 daemon.cpp:95 完全一致
    const QList<KPluginMetaData> plugins = KPluginMetaData::findPlugins(QStringLiteral("ksystemstats"));

    fprintf(stderr, "\n[2] findPlugins(\"ksystemstats\") 找到 %d 个插件:\n", int(plugins.size()));
    for (const auto &md : plugins) {
        fprintf(stderr, "    id=%-8s file=%-40s valid=%d\n",
                qPrintable(md.pluginId()),
                qPrintable(md.fileName()),
                int(md.isValid()));
    }

    KSysGuard::SensorPlugin *npuPlugin = nullptr;

    fprintf(stderr, "\n[3] 逐个实例化（daemon.cpp:100）:\n");
    for (const auto &metaData : plugins) {
        auto provider = KPluginFactory::instantiatePlugin<KSysGuard::SensorPlugin>(metaData);
        if (!provider.plugin) {
            fprintf(stderr, "    x %s: %s\n",
                    qPrintable(metaData.pluginId()),
                    qPrintable(provider.errorString));
            continue;
        }
        fprintf(stderr, "    ok %s  (providerName=\"%s\")\n",
                qPrintable(metaData.pluginId()),
                qPrintable(provider.plugin->providerName()));
        if (provider.plugin->providerName() == QLatin1String("npu")) {
            npuPlugin = provider.plugin;
        } else {
            delete provider.plugin;
        }
    }

    if (!npuPlugin) {
        fprintf(stderr, "\n结果: 没有找到 npu 插件\n");
        return 1;
    }

    fprintf(stderr, "\n[4] npu 插件暴露的传感器:\n");
    for (auto *container : npuPlugin->containers()) {
        fprintf(stderr, "    container %s / %s\n",
                qPrintable(container->id()), qPrintable(container->name()));
        for (auto *object : container->objects()) {
            fprintf(stderr, "      object %s / %s\n",
                    qPrintable(object->id()), qPrintable(object->name()));
            for (auto *sensor : object->sensors()) {
                fprintf(stderr, "        sensor %-10s %-12s unit=%-4d path=%s\n",
                        qPrintable(sensor->id()),
                        qPrintable(sensor->info().name),
                        int(sensor->info().unit),
                        qPrintable(sensor->path()));
            }
        }
    }

    // ksystemstats 只有在客户端（plasma-systemmonitor）订阅后才会真正读取，
    // 这里模拟客户端的订阅行为，否则 SysFsSensor 不会去读 sysfs。
    fprintf(stderr, "\n[5] 订阅所有传感器（模拟客户端请求）...\n");
    for (auto *container : npuPlugin->containers()) {
        for (auto *object : container->objects()) {
            for (auto *sensor : object->sensors()) {
                sensor->subscribe();
                fprintf(stderr, "    subscribed %s (isSubscribed=%d)\n",
                        qPrintable(sensor->id()), int(sensor->isSubscribed()));
            }
        }
    }

    fprintf(stderr, "\n[6] 连续 3 次 update()，观察真实取值:\n");
    for (int i = 0; i < 3; ++i) {
        npuPlugin->update();
        for (auto *container : npuPlugin->containers()) {
            for (auto *object : container->objects()) {
                for (auto *sensor : object->sensors()) {
                    fprintf(stderr, "    #%d %-10s = %s\n",
                            i,
                            qPrintable(sensor->id()),
                            qPrintable(sensor->value().toString()));
                }
            }
        }
        if (i < 2) {
            QThread::msleep(1100);
        }
    }

    fprintf(stderr, "\n结果: 插件工作正常\n");
    delete npuPlugin;
    return 0;
}

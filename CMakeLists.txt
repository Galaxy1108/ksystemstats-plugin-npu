cmake_minimum_required(VERSION 3.20)
project(ksystemstats_plugin_npu VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network DBus)

# 注意：这里用的是各自的包名（KF6CoreAddons / KF6I18n），
# 而不是 `find_package(KF6 COMPONENTS ...)`。
# 后者需要 extra-cmake-modules 生成的 KF6 元包配置，Arch 上未安装 ECM 时不存在。
find_package(KF6CoreAddons REQUIRED)
find_package(KF6I18n REQUIRED)

find_package(KSysGuard REQUIRED)

add_library(ksystemstats_plugin_npu MODULE NpuPlugin.cpp)

target_link_libraries(ksystemstats_plugin_npu PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::DBus
    KF6::CoreAddons
    KF6::I18n
    KSysGuard::SystemStats
)

set_target_properties(ksystemstats_plugin_npu PROPERTIES PREFIX "")

include(GNUInstallDirs)

# ksystemstats 从 Qt 插件目录下的 ksystemstats/ 子目录加载插件。
# 文件名必须遵循 ksystemstats_plugin_<providerName>.so 的约定。
install(TARGETS ksystemstats_plugin_npu
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt6/plugins/ksystemstats)

# 仅用于验证的加载器：模拟 ksystemstats 的加载方式实例化插件并读数。
add_executable(loadtest loadtest.cpp)
target_link_libraries(loadtest PRIVATE Qt6::Core Qt6::DBus KF6::CoreAddons KSysGuard::SystemStats)

#pragma once

namespace RF::PluginEngine {
class PluginFactoryRegistry;
class IPlugin;
class PluginCreationContext;
} // namespace RF::PluginEngine

namespace View3DApplication {

// 宿主自带停靠面板（插件管理、日志）不属于任何插件包，但同样由 ui_config 声明。
// 一个包只能有一个 dockwindow 工厂，所以这里按面板名字分派。
void registerHostDockFactory(RF::PluginEngine::PluginFactoryRegistry& registry);

RF::PluginEngine::IPlugin* createPluginManagerDock(
    const RF::PluginEngine::PluginCreationContext& context);

} // namespace View3DApplication


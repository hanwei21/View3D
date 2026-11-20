#pragma once

namespace RF::PluginEngine {
class IPlugin;
class PluginCreationContext;
class PluginFactoryRegistry;
} // namespace RF::PluginEngine

namespace View3DApplication {

// 插件加载状态面板；与日志面板一起由 registerHostDockFactory 按名字分派创建。
RF::PluginEngine::IPlugin* createPluginManagerDock(
    const RF::PluginEngine::PluginCreationContext& context);

} // namespace View3DApplication

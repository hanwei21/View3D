#pragma once

#include <IDockWindow.h>

#include <QString>

#include <memory>

namespace RF::PluginEngine {
class IApplication;
class IAppLogManager;
class PluginCreationContext;
} // namespace RF::PluginEngine

namespace View3DApplication {

class LogView;

// 日志停靠面板：订阅框架 AppLog 的聚合记录口与 Qt 的 qWarning/qCritical，
// 支持按级别与文本过滤。它是宿主系统组件，不属于任何插件包（与插件管理面板同级）。
class LogDock final : public RF::PluginEngine::IDockWindow {
  public:
    LogDock(const RF::PluginEngine::PluginCreationContext& context);
    ~LogDock() override;

    QString getCaption() const override { return m_caption; }
    RF::PluginEngine::DockingStyle getDefaultDock() const override
    {
        return RF::PluginEngine::DockingStyle::Bottom;
    }
    bool getInitCreate() const override { return true; }
    QString getName() const override { return m_name; }
    void onActive(bool) override {}
    void onCreate(RF::PluginEngine::IApplication* application) override;
    void onDestroy() override;
    QWidget* getWidget() const override;

    // 冒烟检查用：不操作 UI 也能确认落点接通、过滤生效。
    int recordCount() const;
    int visibleLineCount() const;
    bool bufferContains(const QString& needle) const;

  private:
    void attach();
    void detach();

    QString m_name;
    QString m_caption;
    RF::PluginEngine::IApplication* m_application = nullptr;
    std::unique_ptr<LogView> m_view;
    int m_subscribeToken = -1;
};

RF::PluginEngine::IPlugin* createLogDock(const RF::PluginEngine::PluginCreationContext& context);

// 把 Qt 自己的 qWarning/qCritical 接进同一条日志管道（保留原 stderr 输出）。
// 由宿主在启动时安装、退出前卸载。
void installQtMessageLogBridge(RF::PluginEngine::IAppLogManager* manager);
void uninstallQtMessageLogBridge();

} // namespace View3DApplication

#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include "PEGlobals.h"
#include "IApplication.h"
#include "IPluginContainer.h"
#include "IRecentFileManager.h"
#include "IAppLogManager.h"
#include "IStatesManager.h"
#include "IDynamicMenuManager.h"
#include "IPermissionManager.h"

namespace RF {
namespace PluginEngine {

class PLUGIN_ENGINE_EXPORT Application : public IApplication {
private:
    IPluginContainer* m_pluginContainer;
    IContentsView* m_activeContentsView;
    IRecentFileManager* m_recentFileManager;
    IAppLogManager* m_appLogManager;
    IStatesManager* m_stateManager;
    class IMessageCenter* m_messageCenter;
    class IWorkSpace* m_workSpace;
    IDynamicMenuManager* m_dynamicMenuManager;
    QString m_title;
    QStringList m_args;
    bool m_tabbedMdi = true;
    IPermissionManager* m_permissionManager;

public:
    Application();
    virtual ~Application();

    IPluginContainer* getPluginContainer() override { return m_pluginContainer; }
    void setPluginContainer(IPluginContainer* container) { m_pluginContainer = container; }

    IContentsView* getActiveContentsView() override { return m_activeContentsView; }
    void setActiveContentsView(IContentsView* view) { m_activeContentsView = view; }

    IRecentFileManager* getRecentFileManager() override { return m_recentFileManager; }
    IAppLogManager* getAppLogManager() override { return m_appLogManager; }
    IStatesManager* getStateManager() override { return m_stateManager; }
    IMessageCenter* getMessageCenter() override { return m_messageCenter; }
    IWorkSpace* getWorkSpace() override { return m_workSpace; }
    IDynamicMenuManager* getDynamicMenuManager() override { return m_dynamicMenuManager; }

    void closeApplication() override;

    QString getTitle() const override { return m_title; }
    void setTitle(const QString& title) override;
    bool getTabbedMdi() const override { return m_tabbedMdi; }
    void setTabbedMdi(bool tabbedMdi) override;
    QStringList getArgs() const override { return m_args; }
    void setArgs(const QStringList& args) override { m_args = args; }

    void applicationLoaded();
    void onApplicationClosing(ApplicationClosingEventArgs& args);
    void onApplicationClosed(ApplicationClosedEventArgs& args);
};

} // namespace PluginEngine
} // namespace RF


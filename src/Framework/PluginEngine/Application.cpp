#include "Application.h"
#include "RecentFileManager.h"
#include "AppLogManager.h"
#include "StatesManager.h"
#include "DynamicMenuManager.h"
#include "MessageCenter.h"
#include "WorkSpace.h"

namespace RF {
namespace PluginEngine {

Application::Application()
    : m_pluginContainer(nullptr)
    , m_activeContentsView(nullptr)
    , m_permissionManager(nullptr)
{
    m_recentFileManager = new RecentFileManager();
    m_appLogManager = new AppLogManager();
    m_dynamicMenuManager = new DynamicMenuManager();
    m_stateManager = new StatesManager(this);
    m_messageCenter = new MessageCenter();
    m_workSpace = new WorkSpace(m_messageCenter);
}

Application::~Application() {
    delete m_recentFileManager;
    delete m_appLogManager;
    delete m_dynamicMenuManager;
    delete m_stateManager;
    delete m_workSpace;
    delete m_messageCenter;
}

void Application::closeApplication() {
    ApplicationClosingEventArgs args;
    if (ApplicationClosing.hasHandlers()) {
        ApplicationClosing(args);
        if (args.getCancel()) return;
    }
    ApplicationClosedEventArgs closedArgs;
    if (ApplicationClosed.hasHandlers()) {
        ApplicationClosed(closedArgs);
    }
}

void Application::setTitle(const QString& title) {
    m_title = title;
    if (TitleChanged.hasHandlers()) {
        TitleChanged(m_title);
    }
}

void Application::setTabbedMdi(bool tabbedMdi) {
    m_tabbedMdi = tabbedMdi;
    if (TabbedMdiChanged.hasHandlers()) {
        TabbedMdiChanged(m_tabbedMdi);
    }
}

void Application::applicationLoaded() {
    if (ApplicationLoaded.hasHandlers()) {
        ApplicationLoaded();
    }
}

void Application::onApplicationClosing(ApplicationClosingEventArgs& args) {
    if (ApplicationClosing.hasHandlers()) {
        ApplicationClosing(args);
    }
}

void Application::onApplicationClosed(ApplicationClosedEventArgs& args) {
    if (ApplicationClosed.hasHandlers()) {
        ApplicationClosed(args);
    }
}

} // namespace PluginEngine
} // namespace RF


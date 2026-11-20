#pragma once

#include <QString>
#include <QStringList>
#include <QList>
#include "Event.h"
#include "Args.h"
#include "IPlugin.h"

class QMainWindow;

namespace RF {
namespace PluginEngine {

// Forward declarations
class IPluginContainer;
class IContentsView;
class IRecentFileManager;
class IAppLogManager;
class IStatesManager;
class IMessageCenter;
class IWorkSpace;
class IDynamicMenuManager;
class IPermissionManager;
class IUiObjectRegistry;
class IPlugin;

using ApplicationLoadedEvent = Event<>;
using ApplicationClosingEvent = Event<ApplicationClosingEventArgs &>;
using ApplicationClosedEvent = Event<ApplicationClosedEventArgs &>;
using TitleChangedEvent = Event<const QString &>;
using TabbedMdiEvent = Event<bool>;

class IApplication {
  public:
    IApplication() = default;
    virtual ~IApplication() = default;

    virtual IPluginContainer *getPluginContainer() = 0;
    virtual IContentsView *getActiveContentsView() = 0;
    virtual IRecentFileManager *getRecentFileManager() = 0;
    virtual IAppLogManager *getAppLogManager() = 0;
    virtual IStatesManager *getStateManager() = 0;
    virtual IMessageCenter *getMessageCenter() = 0;
    virtual IWorkSpace *getWorkSpace() = 0;
    virtual IDynamicMenuManager *getDynamicMenuManager() = 0;

    virtual void closeApplication() = 0;
    virtual QString getTitle() const = 0;
    virtual void setTitle(const QString &title) = 0;
    virtual bool getTabbedMdi() const = 0;
    virtual void setTabbedMdi(bool tabbedMdi) = 0;
    virtual QStringList getArgs() const = 0;
    virtual void setArgs(const QStringList &args) = 0;

    virtual IUiObjectRegistry *getUiObjectRegistry() {
        return nullptr;
    }
    virtual IPlugin *getPluginObject(const QString &) const {
        return nullptr;
    }
    virtual QMainWindow *getMainWindow() const {
        return nullptr;
    }

    // Events
    ApplicationLoadedEvent ApplicationLoaded;
    ApplicationClosingEvent ApplicationClosing;
    ApplicationClosedEvent ApplicationClosed;
    TitleChangedEvent TitleChanged;
    TabbedMdiEvent TabbedMdiChanged;
};

} // namespace PluginEngine
} // namespace RF

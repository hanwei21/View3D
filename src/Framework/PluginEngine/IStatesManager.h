#pragma once

#include <QMap>
#include <QString>
#include <QVariant>
#include "Event.h"
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

using StateChangedEvent = Event<void*, void*>;

class IStatesManager {
public:
    virtual ~IStatesManager() = default;

    virtual void setEnableState(IPlugin* plugin, void* key, void* value) = 0;
    virtual void setVisibleState(IPlugin* plugin, void* key, void* value) = 0;
    virtual void pluginStateChanged(void* key, void* value) = 0;
    virtual void onStateChanged(void* sender, void* e) = 0;
    virtual void bindEnabled(IPlugin *plugin, const QString &key,
                             const QVariant &expectedValue) = 0;
    virtual void bindVisible(IPlugin *plugin, const QString &key,
                             const QVariant &expectedValue) = 0;
    virtual void changeState(const QString &key, const QVariant &value) = 0;
    virtual QVariant state(const QString &key) const = 0;
    virtual void removePlugin(IPlugin *plugin) = 0;

    StateChangedEvent StateChanged;
    StateChangedEvent BeginStateChanged;
    StateChangedEvent EndStateChanged;
};

} // namespace PluginEngine
} // namespace RF

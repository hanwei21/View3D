#pragma once

#include <map>
#include <QHash>
#include <QVariant>
#include "PEGlobals.h"
#include "IStatesManager.h"

namespace RF {
namespace PluginEngine {

class IApplication;

class PLUGIN_ENGINE_EXPORT StatesManager : public IStatesManager {
private:
    IApplication* m_app;

    struct KeyValue {
        void* key;
        void* value;
        KeyValue(void* k, void* v) : key(k), value(v) {}
        bool operator<(const KeyValue& other) const {
            return key < other.key || (key == other.key && value < other.value);
        }
    };

    std::map<KeyValue, IPlugin*> m_enableKeyValue2Plugin;
    std::map<KeyValue, IPlugin*> m_visibleKeyValue2Plugin;
    struct Binding {
        IPlugin *plugin = nullptr;
        QVariant expected;
    };
    QHash<QString, QList<Binding>> m_enabledBindings;
    QHash<QString, QList<Binding>> m_visibleBindings;
    QHash<QString, QVariant> m_states;

public:
    StatesManager(IApplication* app);
    virtual ~StatesManager() = default;

    void setEnableState(IPlugin* plugin, void* key, void* value) override;
    void setVisibleState(IPlugin* plugin, void* key, void* value) override;
    void pluginStateChanged(void* key, void* value) override;
    void onStateChanged(void* sender, void* e) override;
    void bindEnabled(IPlugin *plugin, const QString &key,
                     const QVariant &expectedValue) override;
    void bindVisible(IPlugin *plugin, const QString &key,
                     const QVariant &expectedValue) override;
    void changeState(const QString &key, const QVariant &value) override;
    QVariant state(const QString &key) const override;
    void removePlugin(IPlugin *plugin) override;
};

} // namespace PluginEngine
} // namespace RF

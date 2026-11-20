#include "StatesManager.h"
#include "IApplication.h"
#include "IPluginContainer.h"
#include <algorithm>

namespace RF {
namespace PluginEngine {

StatesManager::StatesManager(IApplication* app) : m_app(app) {}

void StatesManager::setEnableState(IPlugin* plugin, void* key, void* value) {
    if (plugin != nullptr && key != nullptr && value != nullptr) {
        m_enableKeyValue2Plugin[KeyValue(key, value)] = plugin;
    }
}

void StatesManager::setVisibleState(IPlugin* plugin, void* key, void* value) {
    if (plugin != nullptr && key != nullptr && value != nullptr) {
        m_visibleKeyValue2Plugin[KeyValue(key, value)] = plugin;
    }
}

void StatesManager::pluginStateChanged(void* key, void* value) {
    if (BeginStateChanged.hasHandlers())
        BeginStateChanged(key, value);

    for (auto& kvp : m_enableKeyValue2Plugin) {
        if (kvp.first.key == key)
            m_app->getPluginContainer()->pluginEnable(kvp.second, kvp.first.value == value);
    }
    for (auto& kvp : m_visibleKeyValue2Plugin) {
        if (kvp.first.key == key)
            m_app->getPluginContainer()->pluginVisible(kvp.second, kvp.first.value == value);
    }

    if (StateChanged.hasHandlers())
        StateChanged(key, value);

    if (EndStateChanged.hasHandlers())
        EndStateChanged(key, value);
}

void StatesManager::onStateChanged(void* sender, void* e) {
    if (BeginStateChanged.hasHandlers())
        BeginStateChanged(sender, e);
    if (StateChanged.hasHandlers())
        StateChanged(sender, e);
    if (EndStateChanged.hasHandlers())
        EndStateChanged(sender, e);
}

void StatesManager::bindEnabled(IPlugin *plugin, const QString &key,
                                const QVariant &expectedValue) {
    if (!plugin || key.isEmpty())
        return;
    m_enabledBindings[key].append({plugin, expectedValue});
    if (m_states.contains(key) && m_app && m_app->getPluginContainer())
        m_app->getPluginContainer()->pluginEnable(plugin, m_states.value(key) == expectedValue);
}

void StatesManager::bindVisible(IPlugin *plugin, const QString &key,
                                const QVariant &expectedValue) {
    if (!plugin || key.isEmpty())
        return;
    m_visibleBindings[key].append({plugin, expectedValue});
    if (m_states.contains(key) && m_app && m_app->getPluginContainer())
        m_app->getPluginContainer()->pluginVisible(plugin, m_states.value(key) == expectedValue);
}

void StatesManager::changeState(const QString &key, const QVariant &value) {
    if (key.isEmpty())
        return;
    m_states.insert(key, value);
    auto *container = m_app ? m_app->getPluginContainer() : nullptr;
    if (container) {
        for (const Binding &binding : m_enabledBindings.value(key))
            container->pluginEnable(binding.plugin, binding.expected == value);
        for (const Binding &binding : m_visibleBindings.value(key))
            container->pluginVisible(binding.plugin, binding.expected == value);
    }
    if (StateChanged.hasHandlers())
        StateChanged(nullptr, nullptr);
}

QVariant StatesManager::state(const QString &key) const {
    return m_states.value(key);
}

void StatesManager::removePlugin(IPlugin *plugin) {
    const auto remove = [plugin](auto &bindings) {
        for (auto iterator = bindings.begin(); iterator != bindings.end();) {
            auto &items = iterator.value();
            items.erase(std::remove_if(items.begin(), items.end(),
                                       [plugin](const Binding &binding) {
                                           return binding.plugin == plugin;
                                       }),
                        items.end());
            if (items.isEmpty())
                iterator = bindings.erase(iterator);
            else
                ++iterator;
        }
    };
    remove(m_enabledBindings);
    remove(m_visibleBindings);
}

} // namespace PluginEngine
} // namespace RF

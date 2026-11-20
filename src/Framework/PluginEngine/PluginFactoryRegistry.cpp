#include "PluginFactoryRegistry.h"

namespace RF {
namespace PluginEngine {

void PluginFactoryRegistry::registerCreator(const QString &pluginId, const QString &factoryId,
                                            const QString &logicalType,
                                            const PluginCreator &creator) {
    const QString key = makeKey(pluginId, factoryId, logicalType);
    m_creators[key] = creator;
    m_creatorOwners[key] = pluginId;
    PluginCreationContext context;
    context.pluginId = pluginId;
    context.factoryId = factoryId;
    context.logicalType = logicalType;
    m_contexts[key] = context;
}

void PluginFactoryRegistry::unregisterPlugin(const QString &pluginId) {
    for (auto iterator = m_creatorOwners.begin(); iterator != m_creatorOwners.end();) {
        if (iterator.value() == pluginId) {
            m_creators.remove(iterator.key());
            m_contexts.remove(iterator.key());
            iterator = m_creatorOwners.erase(iterator);
        } else
            ++iterator;
    }
}

QList<PluginCreationContext>
PluginFactoryRegistry::creationContexts(const QString &logicalType) const {
    QList<PluginCreationContext> result;
    for (auto iterator = m_contexts.cbegin(); iterator != m_contexts.cend(); ++iterator)
        if (iterator.value().logicalType == logicalType)
            result.append(iterator.value());
    return result;
}

bool PluginFactoryRegistry::hasCreator(const QString &pluginId, const QString &factoryId,
                                       const QString &logicalType) const {
    return m_creators.contains(makeKey(pluginId, factoryId, logicalType));
}

IPlugin *PluginFactoryRegistry::create(const PluginCreationContext &context) const {
    const QString key = makeKey(context.pluginId, context.factoryId, context.logicalType);
    auto it = m_creators.constFind(key);
    if (it == m_creators.constEnd()) {
        return nullptr;
    }
    return it.value()(context);
}

QString PluginFactoryRegistry::makeKey(const QString &pluginId, const QString &factoryId,
                                       const QString &logicalType) const {
    return pluginId + QStringLiteral("::") + factoryId + QStringLiteral("::") + logicalType;
}

} // namespace PluginEngine
} // namespace RF

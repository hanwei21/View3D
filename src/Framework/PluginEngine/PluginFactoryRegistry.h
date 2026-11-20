#pragma once

#include <QMap>
#include <QList>
#include <QString>
#include "PEGlobals.h"
#include "IPluginObjectFactory.h"

namespace RF {
namespace PluginEngine {

class PLUGIN_ENGINE_EXPORT PluginFactoryRegistry {
public:
    void registerCreator(const QString& pluginId, const QString& factoryId, const QString& logicalType, const PluginCreator& creator);
    bool hasCreator(const QString& pluginId, const QString& factoryId, const QString& logicalType) const;
    IPlugin* create(const PluginCreationContext& context) const;
    void unregisterPlugin(const QString& pluginId);
    QList<PluginCreationContext> creationContexts(const QString &logicalType) const;

private:
    QString makeKey(const QString& pluginId, const QString& factoryId, const QString& logicalType) const;

private:
    QMap<QString, PluginCreator> m_creators;
    QMap<QString, QString> m_creatorOwners;
    QMap<QString, PluginCreationContext> m_contexts;
};

} // namespace PluginEngine
} // namespace RF

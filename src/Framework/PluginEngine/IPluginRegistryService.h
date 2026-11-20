#pragma once

#include <QString>
#include <QStringList>

namespace RF::PluginEngine {

struct PluginPackageStatus {
    QString id;
    QString path;
    bool required = false;
    bool enabled = true;
    bool loaded = false;
    QString error;
    QStringList requiresPlugins;
    QStringList optionalPlugins;
    QStringList provides;
};

class IPluginRegistryService {
  public:
    virtual ~IPluginRegistryService() = default;
    virtual QList<PluginPackageStatus> packages() const = 0;
};

} // namespace RF::PluginEngine

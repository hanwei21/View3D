#pragma once

#include <QString>

namespace RF::PluginEngine {

class ICommandService {
  public:
    virtual ~ICommandService() = default;
    virtual bool commandChecked(const QString &commandId) const = 0;
    virtual bool commandEnabled(const QString &commandId) const = 0;
};

} // namespace RF::PluginEngine

#pragma once

#include <QString>

namespace RF {
namespace PluginEngine {

class IPermissionManager {
public:
    virtual ~IPermissionManager() = default;

    virtual int login(const QString& user, const QString& password) = 0;
    virtual int checkFunction(const QString& url) = 0;
    virtual int checkData(const QString& url, const QString& mode) = 0;
};

} // namespace PluginEngine
} // namespace RF

#pragma once

#include <QString>
#include "PEGlobals.h"

namespace RF {
namespace PluginEngine {

class PLUGIN_ENGINE_EXPORT CheckMoudle {
public:
    static bool checkModuleFile(const QString& modulePath);
};

class PLUGIN_ENGINE_EXPORT RegisterApp {
public:
    static void registerAppInfo();
    static bool initLicenseKey(const QString& authKey, const QString& mapgisProgramDir);

private:
    static QString md5Encrypt(const QString& cleanStr, bool bFormat);
    static QString getClientInfo();
};

} // namespace PluginEngine
} // namespace RF

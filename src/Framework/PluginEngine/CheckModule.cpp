#include "CheckModule.h"

namespace RF {
namespace PluginEngine {

bool CheckMoudle::checkModuleFile(const QString& modulePath) {
    Q_UNUSED(modulePath);
    return true;
}

void RegisterApp::registerAppInfo() {
}

bool RegisterApp::initLicenseKey(const QString& authKey, const QString& mapgisProgramDir) {
    Q_UNUSED(authKey);
    Q_UNUSED(mapgisProgramDir);
    return true;
}

QString RegisterApp::md5Encrypt(const QString& cleanStr, bool bFormat) {
    Q_UNUSED(bFormat);
    return cleanStr;
}

QString RegisterApp::getClientInfo() {
    return QStringLiteral("Unknown");
}

} // namespace PluginEngine
} // namespace RF

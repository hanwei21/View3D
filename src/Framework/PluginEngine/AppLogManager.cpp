#include "AppLogManager.h"
#include "AppLog.h"

namespace RF {
namespace PluginEngine {

AppLogManager::~AppLogManager() {
    for (IAppLog* logger : m_str2Logger)
        delete logger;
}

IAppLog* AppLogManager::getLogger(const QString& name) {
    auto it = m_str2Logger.find(name);
    if (it != m_str2Logger.end()) {
        return it.value();
    }
    IAppLog* appLog = new AppLog(name, this);
    m_str2Logger[name] = appLog;
    return appLog;
}

void AppLog::forward(AppLogLevel level, const QString& message, const QString& details)
{
    if (m_manager)
        m_manager->emitLog(m_name, level, message, details);
}

} // namespace PluginEngine
} // namespace RF

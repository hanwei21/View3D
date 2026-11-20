#pragma once

#include <QMap>
#include <QString>

#include "AppLog.h"
#include "Event.h"
#include "IAppLogManager.h"
#include "PEGlobals.h"

namespace RF {
namespace PluginEngine {

class PLUGIN_ENGINE_EXPORT AppLogManager : public IAppLogManager {
public:
    AppLogManager() = default;
    ~AppLogManager() override;

    IAppLog* getLogger(const QString& name) override;

    int subscribeLog(const LogRecordHandler& handler) override { return m_records.subscribe(handler); }
    void unsubscribeLog(int token) override { m_records.unsubscribe(token); }

    // AppLog 只认识自己的名字，级别与落点分发由管理器统一做。
    void emitLog(const QString& logger, AppLogLevel level, const QString& message,
                 const QString& details)
    {
        m_records(logger, level, message, details);
    }

private:
    QMap<QString, IAppLog*> m_str2Logger;
    Event<const QString&, AppLogLevel, const QString&, const QString&> m_records;
};

} // namespace PluginEngine
} // namespace RF

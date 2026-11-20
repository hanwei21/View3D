#pragma once

#include <QString>

#include <functional>

#include "IAppLog.h"

namespace RF {
namespace PluginEngine {

class IAppLog;

// 日志记录回调：带 logger 名与级别，消费方（日志面板、落盘）只需订阅一次。
using LogRecordHandler = std::function<void(const QString& logger, AppLogLevel level,
                                            const QString& message, const QString& details)>;

class IAppLogManager {
public:
    virtual ~IAppLogManager() = default;

    virtual IAppLog* getLogger(const QString& name) = 0;

    // logger 是按名字惰性创建的，逐个连接既有遗漏又管不好生命周期，
    // 所以聚合订阅口放在管理器上；返回的 token 用于退订。
    virtual int subscribeLog(const LogRecordHandler& handler) = 0;
    virtual void unsubscribeLog(int token) = 0;
};

} // namespace PluginEngine
} // namespace RF

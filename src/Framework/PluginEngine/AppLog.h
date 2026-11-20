#pragma once

#include <QString>

#include <utility>

#include "IAppLog.h"

namespace RF {
namespace PluginEngine {

class AppLogManager;

// logger 本身不做缓冲：每条都立刻发到实例事件与聚合订阅口，由消费方决定留多少。
class AppLog final : public IAppLog {
public:
    AppLog(QString name, AppLogManager* manager)
        : m_name(std::move(name)), m_manager(manager)
    {
    }
    ~AppLog() override = default;

    void debug(const QString& message) override
    {
        if (DebugEvent.hasHandlers()) DebugEvent(message);
        forward(AppLogLevel::Debug, message, QString());
    }
    void debug(const QString& message, const QString& exception) override
    {
        if (DebugEvent1.hasHandlers()) DebugEvent1(message, exception);
        forward(AppLogLevel::Debug, message, exception);
    }

    void error(const QString& message) override
    {
        if (ErrorEvent.hasHandlers()) ErrorEvent(message);
        forward(AppLogLevel::Error, message, QString());
    }
    void error(const QString& message, const QString& exception) override
    {
        if (ErrorEvent1.hasHandlers()) ErrorEvent1(message, exception);
        forward(AppLogLevel::Error, message, exception);
    }

    void fatal(const QString& message) override
    {
        if (FatalEvent.hasHandlers()) FatalEvent(message);
        forward(AppLogLevel::Fatal, message, QString());
    }
    void fatal(const QString& message, const QString& exception) override
    {
        if (FatalEvent1.hasHandlers()) FatalEvent1(message, exception);
        forward(AppLogLevel::Fatal, message, exception);
    }

    void info(const QString& message) override
    {
        if (InfoEvent.hasHandlers()) InfoEvent(message);
        forward(AppLogLevel::Info, message, QString());
    }
    void info(const QString& message, const QString& exception) override
    {
        if (InfoEvent1.hasHandlers()) InfoEvent1(message, exception);
        forward(AppLogLevel::Info, message, exception);
    }

    void warn(const QString& message) override
    {
        if (WarnEvent.hasHandlers()) WarnEvent(message);
        forward(AppLogLevel::Warn, message, QString());
    }
    void warn(const QString& message, const QString& exception) override
    {
        if (WarnEvent1.hasHandlers()) WarnEvent1(message, exception);
        forward(AppLogLevel::Warn, message, exception);
    }

    bool isDebugEnabled() const override { return true; }
    bool isErrorEnabled() const override { return true; }
    bool isFatalEnabled() const override { return true; }
    bool isInfoEnabled() const override { return true; }
    bool isWarnEnabled() const override { return true; }

private:
    void forward(AppLogLevel level, const QString& message, const QString& details);

    QString m_name;
    AppLogManager* m_manager = nullptr;
};

} // namespace PluginEngine
} // namespace RF

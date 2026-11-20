#pragma once

#include <QString>
#include "Event.h"

namespace RF {
namespace PluginEngine {

enum class AppLogLevel {
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

using DebugHandler = Event<const QString&>;
using DebugHandler1 = Event<const QString&, const QString&>;
using ErrorHandler = Event<const QString&>;
using ErrorHandler1 = Event<const QString&, const QString&>;
using FatalHandler = Event<const QString&>;
using FatalHandler1 = Event<const QString&, const QString&>;
using InfoHandler = Event<const QString&>;
using InfoHandler1 = Event<const QString&, const QString&>;
using WarnHandler = Event<const QString&>;
using WarnHandler1 = Event<const QString&, const QString&>;

class IAppLog {
public:
    virtual ~IAppLog() = default;

    virtual void debug(const QString& message) = 0;
    virtual void debug(const QString& message, const QString& exception) = 0;

    virtual void error(const QString& message) = 0;
    virtual void error(const QString& message, const QString& exception) = 0;

    virtual void fatal(const QString& message) = 0;
    virtual void fatal(const QString& message, const QString& exception) = 0;

    virtual void info(const QString& message) = 0;
    virtual void info(const QString& message, const QString& exception) = 0;

    virtual void warn(const QString& message) = 0;
    virtual void warn(const QString& message, const QString& exception) = 0;

    virtual bool isDebugEnabled() const = 0;
    virtual bool isErrorEnabled() const = 0;
    virtual bool isFatalEnabled() const = 0;
    virtual bool isInfoEnabled() const = 0;
    virtual bool isWarnEnabled() const = 0;

    DebugHandler DebugEvent;
    DebugHandler1 DebugEvent1;
    ErrorHandler ErrorEvent;
    ErrorHandler1 ErrorEvent1;
    FatalHandler FatalEvent;
    FatalHandler1 FatalEvent1;
    InfoHandler InfoEvent;
    InfoHandler1 InfoEvent1;
    WarnHandler WarnEvent;
    WarnHandler1 WarnEvent1;
};

} // namespace PluginEngine
} // namespace RF

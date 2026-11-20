#pragma once

#include "PEGlobals.h"
#include <QMap>
#include <QString>
#include <QVariant>
#include <functional>

namespace RF::PluginEngine {

class IPlugin;

struct Message {
    QString topic;
    IPlugin *sender = nullptr;
    QVariantMap arguments;
    QVariant result;
    bool handled = false;
};

class PLUGIN_ENGINE_EXPORT IMessageCenter {
  public:
    using Token = quint64;
    using Handler = std::function<void(Message &)>;

    virtual ~IMessageCenter() = default;
    virtual Token subscribe(const QString &topic, IPlugin *owner, Handler handler) = 0;
    virtual void unsubscribe(Token token) = 0;
    virtual void unsubscribeOwner(IPlugin *owner) = 0;
    virtual void send(Message &message) = 0;
    virtual void publish(const QString &topic, const QVariantMap &arguments = {},
                         IPlugin *sender = nullptr) = 0;
};

} // namespace RF::PluginEngine

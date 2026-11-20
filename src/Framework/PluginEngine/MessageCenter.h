#pragma once

#include "IMessageCenter.h"
#include <QHash>
#include <QVector>

namespace RF::PluginEngine {

class PLUGIN_ENGINE_EXPORT MessageCenter final : public IMessageCenter {
  public:
    Token subscribe(const QString &topic, IPlugin *owner, Handler handler) override;
    void unsubscribe(Token token) override;
    void unsubscribeOwner(IPlugin *owner) override;
    void send(Message &message) override;
    void publish(const QString &topic, const QVariantMap &arguments = {},
                 IPlugin *sender = nullptr) override;

  private:
    struct Subscription {
        Token token = 0;
        IPlugin *owner = nullptr;
        Handler handler;
    };
    Token m_nextToken = 1;
    QHash<QString, QVector<Subscription>> m_subscriptions;
};

} // namespace RF::PluginEngine

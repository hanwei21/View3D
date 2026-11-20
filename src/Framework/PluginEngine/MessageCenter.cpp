#include "MessageCenter.h"

#include <algorithm>

namespace RF::PluginEngine {

IMessageCenter::Token MessageCenter::subscribe(const QString &topic, IPlugin *owner,
                                               Handler handler) {
    if (topic.isEmpty() || !owner || !handler)
        return 0;
    const Token token = m_nextToken++;
    m_subscriptions[topic].append({token, owner, std::move(handler)});
    return token;
}

void MessageCenter::unsubscribe(Token token) {
    if (!token)
        return;
    for (auto iterator = m_subscriptions.begin(); iterator != m_subscriptions.end();) {
        auto &items = iterator.value();
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [token](const Subscription &item) {
                                       return item.token == token;
                                   }),
                    items.end());
        if (items.isEmpty())
            iterator = m_subscriptions.erase(iterator);
        else
            ++iterator;
    }
}

void MessageCenter::unsubscribeOwner(IPlugin *owner) {
    if (!owner)
        return;
    for (auto iterator = m_subscriptions.begin(); iterator != m_subscriptions.end();) {
        auto &items = iterator.value();
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [owner](const Subscription &item) {
                                       return item.owner == owner;
                                   }),
                    items.end());
        if (items.isEmpty())
            iterator = m_subscriptions.erase(iterator);
        else
            ++iterator;
    }
}

void MessageCenter::send(Message &message) {
    const auto items = m_subscriptions.value(message.topic);
    for (const Subscription &item : items) {
        if (item.handler)
            item.handler(message);
    }
}

void MessageCenter::publish(const QString &topic, const QVariantMap &arguments,
                            IPlugin *sender) {
    Message message;
    message.topic = topic;
    message.sender = sender;
    message.arguments = arguments;
    send(message);
}

} // namespace RF::PluginEngine

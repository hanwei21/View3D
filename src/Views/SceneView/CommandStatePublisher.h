#pragma once
#include "ICommandStatePublisher.h"
#include <QHash>
namespace RF::PluginEngine {
class CommandStatePublisher : public ICommandStatePublisher {
  public:
    CommandState commandState(const QString &id) const override {
        return m_states.value(id, {});
    }
    int subscribeCommandState(Listener l) override {
        const int token = ++m_token;
        m_listeners.insert(token, std::move(l));
        return token;
    }
    void unsubscribeCommandState(int t) override {
        m_listeners.remove(t);
    }

  protected:
    void publishCommandState(const QString &id, const CommandState &s) {
        m_states.insert(id, s);
        for (const auto &listener : m_listeners)
            listener(id, s);
    }

  private:
    int m_token = 0;
    QHash<QString, CommandState> m_states;
    QHash<int, Listener> m_listeners;
};
} // namespace RF::PluginEngine

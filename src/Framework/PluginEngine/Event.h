#pragma once
#include <functional>
#include <vector>
#include <mutex>
#include <memory>

template<typename... Args>
class Event {
public:
    using HandlerType = std::function<void(Args...)>;

private:
    struct Entry {
        int id;
        HandlerType handler;
        Entry(int i, HandlerType h) : id(i), handler(std::move(h)) {}
    };

    std::vector<Entry> m_entries;
    mutable std::mutex m_mutex;
    int m_nextId = 1;

public:
    Event() = default;
    ~Event() = default;

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    // Subscribe: returns a token that can be used to unsubscribe
    int subscribe(const HandlerType& handler) {
        std::lock_guard<std::mutex> lock(m_mutex);
        int id = m_nextId++;
        m_entries.emplace_back(id, handler);
        return id;
    }

    // Unsubscribe by token
    bool unsubscribe(int token) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->id == token) {
                m_entries.erase(it);
                return true;
            }
        }
        return false;
    }

    // Convenience operator+= for subscription (returns token)
    int operator+=(const HandlerType& handler) {
        return subscribe(handler);
    }

    // Fire event
    void operator()(Args... args) const {
        std::vector<HandlerType> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_entries.empty()) return;
            snapshot.reserve(m_entries.size());
            for (const auto& entry : m_entries) {
                snapshot.push_back(entry.handler);
            }
        }

        for (const auto& handler : snapshot) {
            handler(args...);
        }
    }

    size_t handlerCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    bool hasHandlers() const {
        return handlerCount() > 0;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }
};

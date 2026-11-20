#include "RecentFileManager.h"

namespace RF {
namespace PluginEngine {

RecentFileManager::~RecentFileManager() {
    for (IRecentFileGroup* group : m_recentFileGroups)
        delete group;
}

IRecentFileGroup* RecentFileManager::add(const QString& groupKey, const QString& groupTitle, int maxCount) {
    for (auto* g : m_recentFileGroups) {
        if (g->getKey() == groupKey)
            return nullptr;
    }
    auto* group = new RecentFileGroup(groupKey, groupTitle, maxCount);
    m_recentFileGroups.push_back(group);
    return group;
}

bool RecentFileManager::clear() {
    for (IRecentFileGroup* group : m_recentFileGroups)
        delete group;
    m_recentFileGroups.clear();
    return true;
}

IRecentFileGroup* RecentFileManager::insert(int index, const QString& groupKey, const QString& groupTitle, int maxCount) {
    for (auto* g : m_recentFileGroups) {
        if (g->getKey() == groupKey)
            return nullptr;
    }
    if (index < 0)
        index = 0;
    if (index > static_cast<int>(m_recentFileGroups.size()))
        index = static_cast<int>(m_recentFileGroups.size());
    auto* group = new RecentFileGroup(groupKey, groupTitle, maxCount);
    m_recentFileGroups.insert(m_recentFileGroups.begin() + index, group);
    return group;
}

bool RecentFileManager::remove(IRecentFileGroup* group) {
    for (auto it = m_recentFileGroups.begin(); it != m_recentFileGroups.end(); ++it) {
        if (*it == group) {
            delete *it;
            m_recentFileGroups.erase(it);
            return true;
        }
    }
    return false;
}

bool RecentFileManager::remove(const QString& groupKey) {
    for (auto it = m_recentFileGroups.begin(); it != m_recentFileGroups.end(); ++it) {
        if ((*it)->getKey() == groupKey) {
            delete *it;
            m_recentFileGroups.erase(it);
            return true;
        }
    }
    return false;
}

bool RecentFileManager::removeAt(int index) {
    if (index < 0 || index >= static_cast<int>(m_recentFileGroups.size())) return false;
    delete m_recentFileGroups[index];
    m_recentFileGroups.erase(m_recentFileGroups.begin() + index);
    return true;
}

bool RecentFileManager::removeRange(int startIndex, int count) {
    if (startIndex < 0 || count < 0 ||
        startIndex + count > static_cast<int>(m_recentFileGroups.size()))
        return false;
    for (int index = startIndex; index < startIndex + count; ++index)
        delete m_recentFileGroups[index];
    m_recentFileGroups.erase(m_recentFileGroups.begin() + startIndex,
                             m_recentFileGroups.begin() + startIndex + count);
    return true;
}

IRecentFileGroup* RecentFileManager::getGroupByKey(const QString& groupKey) const {
    for (auto* g : m_recentFileGroups) {
        if (g->getKey() == groupKey)
            return g;
    }
    return nullptr;
}

IRecentFileGroup* RecentFileManager::getGroupByIndex(int index) const {
    if (index >= 0 && index < static_cast<int>(m_recentFileGroups.size()))
        return m_recentFileGroups[index];
    return nullptr;
}

} // namespace PluginEngine
} // namespace RF

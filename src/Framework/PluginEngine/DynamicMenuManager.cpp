#include "DynamicMenuManager.h"

namespace RF {
namespace PluginEngine {

DynamicMenuManager::~DynamicMenuManager() {
    for (DynamicMenuGroup* group : m_groups)
        delete group;
}

bool DynamicMenuGroup::add(const QString& caption, void* value, bool isChecked, void* glyph) {
    if (value == nullptr) return false;
    for (auto& item : m_items) {
        if (item.value == value) return false;
    }
    m_items.push_back(DynamicMenuItem(caption, value, isChecked, glyph));
    return true;
}

bool DynamicMenuGroup::clear() {
    m_items.clear();
    return true;
}

bool DynamicMenuGroup::insert(int index, const QString& caption, void* value, bool isChecked, void* glyph) {
    if (value == nullptr) return false;
    for (auto& item : m_items) {
        if (item.value == value) return false;
    }
    if (index < 0)
        index = 0;
    if (index > static_cast<int>(m_items.size()))
        index = static_cast<int>(m_items.size());
    m_items.insert(m_items.begin() + index, DynamicMenuItem(caption, value, isChecked, glyph));
    return true;
}

bool DynamicMenuGroup::remove(void* value) {
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (it->value == value) {
            m_items.erase(it);
            return true;
        }
    }
    return false;
}

bool DynamicMenuGroup::removeAt(int index) {
    if (index < 0 || index >= static_cast<int>(m_items.size())) return false;
    m_items.erase(m_items.begin() + index);
    return true;
}

bool DynamicMenuGroup::removeRange(int startIndex, int count) {
    if (startIndex < 0 || count < 0 ||
        startIndex + count > static_cast<int>(m_items.size()))
        return false;
    m_items.erase(m_items.begin() + startIndex, m_items.begin() + startIndex + count);
    return true;
}

bool DynamicMenuGroup::getChecked(void* value) {
    for (auto& item : m_items) {
        if (item.value == value) return item.checked;
    }
    return false;
}

bool DynamicMenuGroup::setChecked(void* value, bool isChecked) {
    for (auto& item : m_items) {
        if (item.value == value) {
            item.checked = isChecked;
            return true;
        }
    }
    return false;
}

QString DynamicMenuGroup::getCaption(void* value) {
    for (auto& item : m_items) {
        if (item.value == value) return item.caption;
    }
    return QString();
}

bool DynamicMenuGroup::setCaption(void* value, const QString& caption) {
    for (auto& item : m_items) {
        if (item.value == value) {
            item.caption = caption;
            return true;
        }
    }
    return false;
}

void* DynamicMenuGroup::getImage(void* value) {
    for (auto& item : m_items) {
        if (item.value == value) return item.glyph;
    }
    return nullptr;
}

bool DynamicMenuGroup::setImage(void* value, void* image) {
    for (auto& item : m_items) {
        if (item.value == value) {
            item.glyph = image;
            return true;
        }
    }
    return false;
}

void* DynamicMenuGroup::getItem(int index) const {
    if (index >= 0 && index < static_cast<int>(m_items.size()))
        return m_items[index].value;
    return nullptr;
}

IDynamicMenuGroup* DynamicMenuManager::add(const QString& groupKey, const QString& groupTitle) {
    for (auto* g : m_groups) {
        if (g->getKey() == groupKey) return nullptr;
    }
    auto* group = new DynamicMenuGroup(groupKey, groupTitle);
    m_groups.push_back(group);
    return group;
}

bool DynamicMenuManager::clear() {
    for (DynamicMenuGroup* group : m_groups)
        delete group;
    m_groups.clear();
    return true;
}

IDynamicMenuGroup* DynamicMenuManager::insert(int index, const QString& groupKey, const QString& groupTitle) {
    for (auto* g : m_groups) {
        if (g->getKey() == groupKey) return nullptr;
    }
    if (index < 0)
        index = 0;
    if (index > static_cast<int>(m_groups.size()))
        index = static_cast<int>(m_groups.size());
    auto* group = new DynamicMenuGroup(groupKey, groupTitle);
    m_groups.insert(m_groups.begin() + index, group);
    return group;
}

bool DynamicMenuManager::remove(IDynamicMenuGroup* group) {
    for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
        if (*it == static_cast<DynamicMenuGroup*>(group)) {
            delete *it;
            m_groups.erase(it);
            return true;
        }
    }
    return false;
}

bool DynamicMenuManager::remove(const QString& groupKey) {
    for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
        if ((*it)->getKey() == groupKey) {
            delete *it;
            m_groups.erase(it);
            return true;
        }
    }
    return false;
}

bool DynamicMenuManager::removeAt(int index) {
    if (index < 0 || index >= static_cast<int>(m_groups.size())) return false;
    delete m_groups[index];
    m_groups.erase(m_groups.begin() + index);
    return true;
}

bool DynamicMenuManager::removeRange(int startIndex, int count) {
    if (startIndex < 0 || count < 0 ||
        startIndex + count > static_cast<int>(m_groups.size()))
        return false;
    for (int index = startIndex; index < startIndex + count; ++index)
        delete m_groups[index];
    m_groups.erase(m_groups.begin() + startIndex, m_groups.begin() + startIndex + count);
    return true;
}

IDynamicMenuGroup* DynamicMenuManager::getByKey(const QString& groupKey) const {
    for (auto* g : m_groups) {
        if (g->getKey() == groupKey) return g;
    }
    return nullptr;
}

IDynamicMenuGroup* DynamicMenuManager::getByIndex(int index) const {
    if (index >= 0 && index < static_cast<int>(m_groups.size()))
        return m_groups[index];
    return nullptr;
}

} // namespace PluginEngine
} // namespace RF

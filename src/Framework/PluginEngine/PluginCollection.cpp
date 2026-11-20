#include "PluginCollection.h"
#include <algorithm>

namespace RF {
namespace PluginEngine {

int PluginCollection::add(IPlugin* value) {
    m_list.push_back(value);
    return static_cast<int>(m_list.size()) - 1;
}

int PluginCollection::indexOf(IPlugin* value) const {
    for (size_t i = 0; i < m_list.size(); ++i) {
        if (m_list[i] == value)
            return static_cast<int>(i);
    }
    return -1;
}

bool PluginCollection::contains(IPlugin* value) const {
    return indexOf(value) >= 0;
}

void PluginCollection::remove(IPlugin* value) {
    auto it = std::find(m_list.begin(), m_list.end(), value);
    if (it != m_list.end())
        m_list.erase(it);
}

void PluginCollection::insert(int index, IPlugin* value) {
    m_list.insert(m_list.begin() + index, value);
}

IPlugin* PluginCollection::getItem(int index) const {
    if (index >= 0 && index < static_cast<int>(m_list.size()))
        return m_list[index];
    return nullptr;
}

} // namespace PluginEngine
} // namespace RF

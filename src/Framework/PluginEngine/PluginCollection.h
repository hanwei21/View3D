#pragma once

#include <vector>
#include "PEGlobals.h"
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class PLUGIN_ENGINE_EXPORT PluginCollection {
private:
    std::vector<IPlugin*> m_list;

public:
    PluginCollection() = default;

    int add(IPlugin* value);
    int indexOf(IPlugin* value) const;
    bool contains(IPlugin* value) const;
    void remove(IPlugin* value);
    void insert(int index, IPlugin* value);
    int getCount() const { return static_cast<int>(m_list.size()); }

    IPlugin* getItem(int index) const;

    // Iterator support
    using iterator = std::vector<IPlugin*>::iterator;
    using const_iterator = std::vector<IPlugin*>::const_iterator;
    iterator begin() { return m_list.begin(); }
    iterator end() { return m_list.end(); }
    const_iterator begin() const { return m_list.begin(); }
    const_iterator end() const { return m_list.end(); }
};

} // namespace PluginEngine
} // namespace RF

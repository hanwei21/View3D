#pragma once

#include <vector>
#include <QString>
#include "PEGlobals.h"
#include "IRecentFileManager.h"
#include "RecentFileGroup.h"

namespace RF {
namespace PluginEngine {

class PLUGIN_ENGINE_EXPORT RecentFileManager : public IRecentFileManager {
private:
    std::vector<IRecentFileGroup*> m_recentFileGroups;

public:
    RecentFileManager() = default;
    ~RecentFileManager() override;

    IRecentFileGroup* add(const QString& groupKey, const QString& groupTitle, int maxCount) override;
    bool clear() override;
    IRecentFileGroup* insert(int index, const QString& groupKey, const QString& groupTitle, int maxCount) override;
    bool remove(IRecentFileGroup* group) override;
    bool remove(const QString& groupKey) override;
    bool removeAt(int index) override;
    bool removeRange(int startIndex, int count) override;

    int getCount() const override { return static_cast<int>(m_recentFileGroups.size()); }
    IRecentFileGroup* getGroupByKey(const QString& groupKey) const override;
    IRecentFileGroup* getGroupByIndex(int index) const override;
};

} // namespace PluginEngine
} // namespace RF

#pragma once

#include <QString>

namespace RF {
namespace PluginEngine {

class IRecentFileGroup;

class IRecentFileManager {
public:
    virtual ~IRecentFileManager() = default;

    virtual IRecentFileGroup* add(const QString& groupKey, const QString& groupTitle, int maxCount) = 0;
    virtual bool clear() = 0;
    virtual IRecentFileGroup* insert(int index, const QString& groupKey, const QString& groupTitle, int maxCount) = 0;
    virtual bool remove(IRecentFileGroup* group) = 0;
    virtual bool remove(const QString& groupKey) = 0;
    virtual bool removeAt(int index) = 0;
    virtual bool removeRange(int startIndex, int count) = 0;

    virtual int getCount() const = 0;
    virtual IRecentFileGroup* getGroupByKey(const QString& groupKey) const = 0;
    virtual IRecentFileGroup* getGroupByIndex(int index) const = 0;
};

} // namespace PluginEngine
} // namespace RF

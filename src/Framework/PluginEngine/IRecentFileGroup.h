#pragma once

#include <QString>
#include "Event.h"

namespace RF {
namespace PluginEngine {

using RecentFileClickEvent = Event<const QString&>;

class IRecentFileGroup {
public:
    virtual ~IRecentFileGroup() = default;

    virtual bool clear() = 0;
    virtual bool insert(int index, const QString& filePath) = 0;
    virtual bool remove(const QString& filePath) = 0;
    virtual bool removeAt(int index) = 0;
    virtual bool removeRange(int startIndex, int count) = 0;
    virtual bool setChecked(const QString& filePath, bool isChecked) = 0;
    virtual bool getChecked(const QString& filePath) = 0;

    virtual int getMaxCount() const = 0;
    virtual int getCount() const = 0;
    virtual QString getItem(int index) const = 0;
    virtual QString getTitle() const = 0;
    virtual QString getKey() const = 0;

    RecentFileClickEvent RecentFileClick;
};

} // namespace PluginEngine
} // namespace RF

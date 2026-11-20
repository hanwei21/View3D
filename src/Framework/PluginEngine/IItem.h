#pragma once

#include <QString>

namespace RF {
namespace PluginEngine {

class IItem {
public:
    virtual ~IItem() = default;

    virtual bool getGroup() const = 0;
    virtual void setGroup(bool group) = 0;
    virtual QString getKey() const = 0;
    virtual void setKey(const QString& key) = 0;
    virtual bool getShowLargeImage() const = 0;
    virtual void setShowLargeImage(bool showLargeImage) = 0;
};

} // namespace PluginEngine
} // namespace RF

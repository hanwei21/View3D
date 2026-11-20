#pragma once

#include <QList>
#include <QString>

namespace RF {
namespace PluginEngine {

class IItem;

class IBar {
public:
    virtual ~IBar() = default;

    virtual QString getAttachContentsViewKey() const = 0;
    virtual QString getCaption() const = 0;
    virtual QList<IItem*> getIItems() const = 0;
    virtual QString getName() const = 0;
};

} // namespace PluginEngine
} // namespace RF

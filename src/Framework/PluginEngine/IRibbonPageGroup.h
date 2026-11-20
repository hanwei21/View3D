#pragma once

#include <QList>
#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IItem;

class IRibbonPageGroup : public IPlugin {
public:
    virtual ~IRibbonPageGroup() = default;

    virtual QString getClickKey() const = 0;
    virtual QList<IItem*> getIItems() const = 0;
    virtual QString getName() const = 0;
    virtual QString getPageKey() const = 0;
    virtual bool getShowCaptionButton() const = 0;
    virtual QString getText() const = 0;
};

} // namespace PluginEngine
} // namespace RF

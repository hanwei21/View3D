#pragma once

#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IApplication;

class IDropDown : public IPlugin {
public:
    virtual ~IDropDown() = default;

    virtual QString getCaption() const = 0;
    virtual QString getCategory() const = 0;
    virtual bool getEnabled() const = 0;
    virtual QString getMessage() const = 0;
    virtual QString getName() const = 0;
    virtual QString getTooltip() const = 0;
    virtual bool getShowCloseButton() const = 0;
    virtual bool getShowSizeGrip() const = 0;

    virtual void onClick() = 0;
    virtual void onCreate(IApplication* app) = 0;
    virtual void onDestroy() = 0;
};

} // namespace PluginEngine
} // namespace RF


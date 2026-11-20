#pragma once

#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IApplication;

enum class CheckState {
    Unchecked = 0,
    Checked = 1,
    Indeterminate = 2
};

class ICheckBox : public IPlugin {
public:
    virtual ~ICheckBox() = default;

    virtual bool getAllowThreeState() const = 0;
    virtual QString getCaption() const = 0;
    virtual QString getCategory() const = 0;
    virtual bool getEnabled() const = 0;
    virtual CheckState getInitState() const = 0;
    virtual QString getMessage() const = 0;
    virtual QString getName() const = 0;
    virtual QString getTooltip() const = 0;
    virtual int getWidth() const = 0;

    virtual void onCheckStateChanged(CheckState newCheckState, CheckState oldCheckState) = 0;
    virtual void onCreate(IApplication* app) = 0;
    virtual void onDestroy() = 0;
};

} // namespace PluginEngine
} // namespace RF


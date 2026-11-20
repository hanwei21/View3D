#pragma once

#include <QString>

namespace RF {
namespace PluginEngine {

class ISubItem {
public:
    virtual ~ISubItem() = default;

    virtual QString getCaption() const = 0;
    virtual QString getMessage() const = 0;
    virtual QString getName() const = 0;
    virtual QString getTooltip() const = 0;
    virtual bool getGroup() const = 0;
};

} // namespace PluginEngine
} // namespace RF

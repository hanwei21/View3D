#pragma once

#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IApplication;

enum class StaticItemSize {
    None = 0,
    Content = 1,
    Spring = 2
};

enum class StaticAlignment {
    Default = 0,
    Left = 1,
    Right = 2
};

class IStatic : public IPlugin {
public:
    virtual ~IStatic() = default;

    virtual StaticAlignment getAlignment() const = 0;
    virtual StaticItemSize getAutoSize() const = 0;
    virtual QString getCaption() const = 0;
    virtual QString getCategory() const = 0;
    virtual QString getName() const = 0;
    virtual int getWidth() const = 0;

    virtual void onDoubleClick() = 0;
    virtual void onCreate(IApplication* app) = 0;
    virtual void onDestroy() = 0;
};

} // namespace PluginEngine
} // namespace RF


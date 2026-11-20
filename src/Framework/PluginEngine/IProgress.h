#pragma once

#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IApplication;

enum class ProgressAlignment {
    Default = 0,
    Left = 1,
    Right = 2
};

class IProgress : public IPlugin {
public:
    virtual ~IProgress() = default;

    virtual ProgressAlignment getAlignment() const = 0;
    virtual QString getCaption() const = 0;
    virtual QString getCategory() const = 0;
    virtual int getHeight() const = 0;
    virtual int getMaximum() const = 0;
    virtual int getMinimum() const = 0;
    virtual QString getName() const = 0;
    virtual int getStep() const = 0;
    virtual int getWidth() const = 0;

    virtual void onCreate(IApplication* app) = 0;
    virtual void onDestroy() = 0;
};

} // namespace PluginEngine
} // namespace RF


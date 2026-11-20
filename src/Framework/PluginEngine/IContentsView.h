#pragma once

#include <QString>
#include "IPlugin.h"

class QWidget;


namespace RF {
namespace PluginEngine {

class IApplication;

class IContentsView : public IPlugin {
public:
    virtual ~IContentsView() = default;

    virtual QString getCaption() const = 0;
    virtual bool getControlBox() const = 0;
    virtual bool getInitCreate() const = 0;
    virtual QString getName() const = 0;

    virtual void onActive(bool isActive) = 0;
    virtual void onClose() = 0;
    virtual void onDestroy() = 0;
    virtual void onCreate(IApplication* app) = 0;
    virtual QWidget* getWidget() const = 0;
};

} // namespace PluginEngine
} // namespace RF






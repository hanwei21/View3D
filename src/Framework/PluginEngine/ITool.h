#pragma once

#include <QString>
#include "IPlugin.h"

namespace RF {
namespace PluginEngine {

class IApplication;

class ITool : public IPlugin {
public:
    virtual ~ITool() = default;

    virtual QString getCaption() const = 0;
    virtual QString getCategory() const = 0;
    virtual bool getChecked() const = 0;
    virtual int getCursor() const = 0;
    virtual bool getEnabled() const = 0;
    virtual int getHelpContextId() const = 0;
    virtual QString getHelpFile() const = 0;
    virtual QString getMessage() const = 0;
    virtual QString getName() const = 0;
    virtual QString getTooltip() const = 0;

    virtual bool deactivate() = 0;
    virtual void onClick() = 0;
    virtual bool onContextMenu(int x, int y) = 0;
    virtual void onCreate(IApplication* app) = 0;
    virtual void onDestroy() = 0;
    virtual void onDblClick() = 0;
    virtual void onKeyDown(int keyCode, int shift) = 0;
    virtual void onKeyUp(int keyCode, int shift) = 0;
    virtual void onMouseDown(int button, int shift, int x, int y) = 0;
    virtual void onMouseMove(int button, int shift, int x, int y) = 0;
    virtual void onMouseUp(int button, int shift, int x, int y) = 0;
    virtual void refresh(int hDC) = 0;
};

} // namespace PluginEngine
} // namespace RF


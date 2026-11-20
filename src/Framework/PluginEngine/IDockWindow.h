#pragma once

#include <QString>
#include "IPlugin.h"

class QWidget;


namespace RF {
namespace PluginEngine {

class IApplication;

enum class DockingStyle {
    Float = 0,
    Top = 1,
    Bottom = 2,
    Left = 3,
    Right = 4
};

class IDockWindow : public IPlugin {
public:
    virtual ~IDockWindow() = default;

    virtual QString getCaption() const = 0;
    virtual DockingStyle getDefaultDock() const = 0;
    virtual bool getInitCreate() const = 0;
    virtual QString getName() const = 0;

    virtual void onActive(bool isActive) = 0;
    virtual void onCreate(IApplication* app) = 0;
    virtual void onDestroy() = 0;
    virtual QWidget* getWidget() const = 0;
    // 内容是否交给停靠框架的滚动区托管（默认 true，保持既有面板的手感）。
    // 多栏面板要铺满停靠区、自己管栏内滚动，就返回 false：否则内容按
    // 自身最小尺寸摆放，停靠区一大就只剩左上角一小块。
    virtual bool getScrollable() const { return true; }
};

} // namespace PluginEngine
} // namespace RF





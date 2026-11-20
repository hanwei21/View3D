#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include "../ICommand.h"
#include "../IContentsView.h"
#include "../IDockWindow.h"
#include "../IStatusBar.h"
#include "../IRibbonPage.h"
#include "../IRibbonPageGroup.h"
#include "../IQuickAccessToolBar.h"
#include "../IApplicationMenu.h"
#include "../IRightTopToolBar.h"
#include "../IFloatingHost.h"
#include "../IWelcomeScreen.h"
#include "../IStatic.h"
#include "../IProgress.h"

namespace RF {
namespace PluginEngine {

class ConfigCommand : public ICommand {
public:
    QString name;
    QString caption;
    QString category;
    QString message;
    QString tooltip;
    bool checked = false;
    bool enabled = true;

    QString getCaption() const override { return caption; }
    QString getCategory() const override { return category; }
    bool getChecked() const override { return checked; }
    bool getEnabled() const override { return enabled; }
    QString getMessage() const override { return message; }
    QString getName() const override { return name; }
    QString getTooltip() const override { return tooltip; }
    void onClick() override {}
    void onCreate(IApplication* app) override { Q_UNUSED(app); }
    void onDestroy() override {}
};

class ConfigDockWindow : public IDockWindow {
public:
    QString name;
    QString caption;
    DockingStyle defaultDock = DockingStyle::Left;
    bool initCreate = true;

    QString getCaption() const override { return caption; }
    DockingStyle getDefaultDock() const override { return defaultDock; }
    bool getInitCreate() const override { return initCreate; }
    QString getName() const override { return name; }
    void onActive(bool isActive) override { Q_UNUSED(isActive); }
    void onCreate(IApplication* app) override { Q_UNUSED(app); }
    void onDestroy() override {}
    QWidget* getWidget() const override { return nullptr; }
    
};

class ConfigContentsView : public IContentsView {
public:
    QString name;
    QString caption;
    bool controlBox = true;
    bool initCreate = true;

    QString getCaption() const override { return caption; }
    bool getControlBox() const override { return controlBox; }
    bool getInitCreate() const override { return initCreate; }
    QString getName() const override { return name; }
    void onActive(bool isActive) override { Q_UNUSED(isActive); }
    void onClose() override {}
    void onCreate(IApplication* app) override { Q_UNUSED(app); }
    void onDestroy() override {}
    QWidget* getWidget() const override { return nullptr; }
};

class ConfigWelcomeScreen : public IWelcomeScreen {
public:
    int result = 0;

    int getResult() const override { return result; }
    void close() override {}
    void show() override {}
    void onCreate(IApplication* app) override { Q_UNUSED(app); }
    void onDestroy() override {}
};

class ConfigStatic : public IStatic {
public:
    QString name;
    QString caption;
    QString category;
    int width = 0;
    StaticAlignment alignment = StaticAlignment::Default;
    StaticItemSize autoSize = StaticItemSize::Content;

    StaticAlignment getAlignment() const override { return alignment; }
    StaticItemSize getAutoSize() const override { return autoSize; }
    QString getCaption() const override { return caption; }
    QString getCategory() const override { return category; }
    QString getName() const override { return name; }
    int getWidth() const override { return width; }
    void onDoubleClick() override {}
    void onCreate(IApplication* app) override { Q_UNUSED(app); }
    void onDestroy() override {}
};

class ConfigProgress : public IProgress {
public:
    QString name;
    QString caption;
    QString category;
    int width = 0;
    int minimum = 0;
    int maximum = 100;
    int step = 1;
    int height = 0;
    ProgressAlignment alignment = ProgressAlignment::Default;

    ProgressAlignment getAlignment() const override { return alignment; }
    QString getCaption() const override { return caption; }
    QString getCategory() const override { return category; }
    int getHeight() const override { return height; }
    int getMaximum() const override { return maximum; }
    int getMinimum() const override { return minimum; }
    QString getName() const override { return name; }
    int getStep() const override { return step; }
    int getWidth() const override { return width; }
    void onCreate(IApplication* app) override { Q_UNUSED(app); }
    void onDestroy() override {}
};

class ConfigStatusBar : public IStatusBar {
public:
    QString name;
    QString caption;
    QString attachContentsViewKey;
    bool isDefault = false;
    QList<IItem*> items;

    QString getAttachContentsViewKey() const override { return attachContentsViewKey; }
    QString getCaption() const override { return caption; }
    QList<IItem*> getIItems() const override { return items; }
    QString getName() const override { return name; }
    bool getIsDefaultStatusBar() const override { return isDefault; }
};

class ConfigRibbonPage : public IRibbonPage {
public:
    QString name;
    QString text;
    QString categoryKey;
    bool selected = false;

    QString getCategoryKey() const override { return categoryKey; }
    QString getName() const override { return name; }
    QString getText() const override { return text; }
    bool getSelected() const override { return selected; }
};

class ConfigRibbonPageGroup : public IRibbonPageGroup {
public:
    QString name;
    QString text;
    QString pageKey;
    QString clickKey;
    bool showCaptionButton = false;
    QList<IItem*> items;

    QString getClickKey() const override { return clickKey; }
    QList<IItem*> getIItems() const override { return items; }
    QString getName() const override { return name; }
    QString getPageKey() const override { return pageKey; }
    bool getShowCaptionButton() const override { return showCaptionButton; }
    QString getText() const override { return text; }
};

class ConfigQuickAccessToolBar : public IQuickAccessToolBar {
public:
    QString name;
    QString caption;
    QString attachContentsViewKey;
    QList<IItem*> items;

    QString getAttachContentsViewKey() const override { return attachContentsViewKey; }
    QString getCaption() const override { return caption; }
    QList<IItem*> getIItems() const override { return items; }
    QString getName() const override { return name; }
};

class ConfigApplicationMenu : public IApplicationMenu {
public:
    QString name;
    QString caption;
    QString image;

    QString getName() const override { return name; }
    QString getCaption() const override { return caption; }
    QString getImage() const override { return image; }
};

class ConfigRightTopToolBar : public IRightTopToolBar {
public:
    QString name;
    QString caption;
    QString attachContentsViewKey;
    QList<IItem*> items;

    QString getAttachContentsViewKey() const override { return attachContentsViewKey; }
    QString getCaption() const override { return caption; }
    QList<IItem*> getIItems() const override { return items; }
    QString getName() const override { return name; }
};

class ConfigFloatingHost : public IFloatingHost {
public:
    QString name;
    QString text;

    QString getName() const override { return name; }
    QString getText() const override { return text; }
};

} // namespace PluginEngine
} // namespace RF


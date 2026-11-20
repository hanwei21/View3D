#pragma once

#include <QString>
#include <QStringList>
#include <QMap>
#include <QtGlobal>
#include <typeinfo>
#include "Event.h"
#include "Args.h"
#include "IPlugin.h"
#include "IContentsView.h"

namespace RF {
namespace PluginEngine {

class ICommand;
class ITool;
class IEditCombobox;
class ICheckBox;
class IGallery;
class IStatic;
class IProgress;
class IDropDown;
class IDockWindow;
class IStatusBar;
class IRibbonPageCategory;
class IRibbonPage;
class IRibbonPageGroup;
class IWelcomeScreen;
class ISubItem;
class IFunction;
class IQuickAccessToolBar;
class IApplicationMenu;
class IRightTopToolBar;
class IFloatingHost;

class PluginEventArgs {
  private:
    IPlugin *m_plugin;
    void *m_parameter;
    void *m_parameter2;

  public:
    PluginEventArgs() : m_plugin(nullptr), m_parameter(nullptr), m_parameter2(nullptr) {
    }
    PluginEventArgs(IPlugin *plugin)
        : m_plugin(plugin), m_parameter(nullptr), m_parameter2(nullptr) {
    }
    PluginEventArgs(IPlugin *plugin, void *parameter)
        : m_plugin(plugin), m_parameter(parameter), m_parameter2(nullptr) {
    }
    PluginEventArgs(IPlugin *plugin, void *parameter, void *parameter2)
        : m_plugin(plugin), m_parameter(parameter), m_parameter2(parameter2) {
    }

    IPlugin *getPlugin() const {
        return m_plugin;
    }
    void *getParameter() const {
        return m_parameter;
    }
    void *getParameter2() const {
        return m_parameter2;
    }
};

class CanPluginEnableEventArgs {
  private:
    IPlugin *m_plugin;
    bool m_canEnable;

  public:
    CanPluginEnableEventArgs(IPlugin *plugin) : m_plugin(plugin), m_canEnable(false) {
    }

    IPlugin *getPlugin() const {
        return m_plugin;
    }
    bool getCanEnable() const {
        return m_canEnable;
    }
    void setCanEnable(bool canEnable) {
        m_canEnable = canEnable;
    }
};

class RibbonPageEventArgs {
  private:
    QString m_ribbonPageText;
    QString m_ribbonPageName;
    QString m_ribbonPageCategoryText;
    QString m_ribbonPageCategoryName;

  public:
    RibbonPageEventArgs() = default;
    RibbonPageEventArgs(const QString &ribbonPageText, const QString &ribbonPageCategoryText)
        : m_ribbonPageText(ribbonPageText), m_ribbonPageCategoryText(ribbonPageCategoryText) {
    }
    RibbonPageEventArgs(const QString &ribbonPageName, const QString &ribbonPageText,
                        const QString &ribbonPageCategoryName,
                        const QString &ribbonPageCategoryText)
        : m_ribbonPageText(ribbonPageText), m_ribbonPageName(ribbonPageName),
          m_ribbonPageCategoryText(ribbonPageCategoryText),
          m_ribbonPageCategoryName(ribbonPageCategoryName) {
    }

    const QString &getRibbonPageName() const {
        return m_ribbonPageName;
    }
    void setRibbonPageName(const QString &v) {
        m_ribbonPageName = v;
    }
    const QString &getRibbonPageText() const {
        return m_ribbonPageText;
    }
    void setRibbonPageText(const QString &v) {
        m_ribbonPageText = v;
    }
    const QString &getRibbonPageCategoryName() const {
        return m_ribbonPageCategoryName;
    }
    void setRibbonPageCategoryName(const QString &v) {
        m_ribbonPageCategoryName = v;
    }
    const QString &getRibbonPageCategoryText() const {
        return m_ribbonPageCategoryText;
    }
    void setRibbonPageCategoryText(const QString &v) {
        m_ribbonPageCategoryText = v;
    }
};

class PluginStateEventArgs {
  private:
    QString m_pluginTypeName;

  public:
    PluginStateEventArgs(const QString &pluginTypeName) : m_pluginTypeName(pluginTypeName) {
    }
    const QString &getPluginTypeName() const {
        return m_pluginTypeName;
    }
};

class SetPluginVisibleEventArgs : public PluginStateEventArgs {
  private:
    bool m_visible;

  public:
    SetPluginVisibleEventArgs(const QString &pluginTypeName)
        : PluginStateEventArgs(pluginTypeName), m_visible(false) {
    }
    bool getVisible() const {
        return m_visible;
    }
    void setVisible(bool v) {
        m_visible = v;
    }
};

class SetPluginEnableEventArgs : public PluginStateEventArgs {
  private:
    bool m_enable;

  public:
    SetPluginEnableEventArgs(const QString &pluginTypeName)
        : PluginStateEventArgs(pluginTypeName), m_enable(false) {
    }
    bool getEnable() const {
        return m_enable;
    }
    void setEnable(bool v) {
        m_enable = v;
    }
};

using PluginLoadedEvent = Event<IPlugin *>;
using PluginUnLoadedEvent = Event<IPlugin *>;
using ContentsViewChangedEvent = Event<IContentsView *, IContentsView *>;
using DockWindowChangedEvent = Event<IDockWindow *, IDockWindow *>;
using ContentsViewClosingEvent = Event<IContentsView *, ContentsViewClosingEventArgs &>;
using ContentsViewClosedEvent = Event<IContentsView *>;
using DockWindowRequestEvent = Event<IDockWindow *>;
using ContentsViewCreateRequestEvent = Event<IContentsView *, const QString &, const QString &>;
using ContentsViewRequestEvent = Event<IContentsView *>;
using ContentsViewSetTextRequestEvent = Event<IContentsView *, const QString &>;
using ContentsViewGetTextRequestEvent = Event<IContentsView *, QString &>;
using CommandClickedEvent = Event<ICommand *>;
using CanPluginEnableEvent = Event<void *, CanPluginEnableEventArgs &>;
using SelectedRibbonPageChangedEvent = Event<void *, RibbonPageEventArgs &>;

using PluginAddedEvent = Event<void *, PluginEventArgs &>;
using PluginRemovedEvent = Event<void *, PluginEventArgs &>;
using PluginVisibleChangedEvent = Event<IPlugin *, bool>;
using PluginEnableChangedEvent = Event<IPlugin *, bool>;

class IPluginContainer {
  public:
    IPluginContainer() = default;
    virtual ~IPluginContainer() = default;

    virtual void pluginVisible(IPlugin *plugin, bool visible) = 0;
    virtual void pluginVisible(const QString &pluginTypeName, bool visible) = 0;
    virtual bool pluginIsVisible(IPlugin *plugin) = 0;
    virtual void pluginEnable(IPlugin *plugin, bool enable) = 0;
    virtual void pluginEnable(const QString &pluginTypeName, bool enable) = 0;
    virtual bool pluginIsEnable(IPlugin *plugin) = 0;
    // 服务注册表：发布时必须登记接口类型，解析时按类型校验，避免错误转型。
    virtual void publishObject(const QString &name, void *object, const std::type_info &type,
                               IPlugin *owner = nullptr) = 0;
    virtual void unpublishObject(const QString &name, void *object) = 0;
    virtual void unpublishObjectsOwnedBy(IPlugin *owner) = 0;
    virtual void *object(const QString &name) const = 0;
    virtual const std::type_info *objectType(const QString &name) const = 0;

    template <typename T>
    void publishAs(const QString &name, T *object, IPlugin *owner = nullptr) {
        publishObject(name, static_cast<void *>(object), typeid(T), owner);
    }

    template <typename T> T *objectAs(const QString &name) const {
        void *found = object(name);
        if (!found)
            return nullptr;
        const std::type_info *type = objectType(name);
        if (!type || *type != typeid(T)) {
            qWarning("View3D service type mismatch: name=%s published=%s requested=%s",
                     name.toUtf8().constData(), type ? type->name() : "<null>",
                     typeid(T).name());
            return nullptr;
        }
        return static_cast<T *>(found);
    }

    virtual void selectPage(IRibbonPage *ribbonPage) = 0;
    virtual void setCommandCheckState(ICommand *command, bool checkState) = 0;
    virtual void setCheckBoxState(ICheckBox *checkBox, int newCheckState) = 0;

    virtual IDockWindow *createDockWindow(const QString &typeName) = 0;
    virtual void destroyDockWindow(IDockWindow *dockWindow) = 0;
    virtual void activeDockWindow(IDockWindow *dockWindow) = 0;

    virtual IContentsView *createContentsView(const QString &typeName, const QString &key) = 0;
    virtual IContentsView *createContentsView(const QString &typeName, const QString &key,
                                              const QString &caption) = 0;
    virtual void activeContentsView(IContentsView *contentsView) = 0;
    virtual void setContentsViewText(IContentsView *contentsView, const QString &text) = 0;
    virtual QString getContentsViewText(IContentsView *contentsView) = 0;
    virtual void closeContentsView(IContentsView *contentsView) = 0;

    virtual void setStaticCaption(IStatic *staticPlugin, const QString &caption) = 0;
    virtual void setProgressValue(IProgress *progress, int value) = 0;
    virtual void setGalleryItemCheckState(void *galleryItem, bool checkState) = 0;
    virtual void performStepProgress(IProgress *progress) = 0;
    virtual void incrementProgress(IProgress *progress, int value) = 0;

    virtual void setEditComboboxText(IEditCombobox *editCombobox, const QString &text) = 0;
    virtual void setEditComboboxValue(IEditCombobox *editCombobox, void *value) = 0;
    virtual int addComboBoxItem(IEditCombobox *editCombobox, void *comboBoxItem) = 0;
    virtual void removeComboBoxItem(IEditCombobox *editCombobox, void *value) = 0;
    virtual void clearComboBoxItem(IEditCombobox *editCombobox) = 0;

    virtual void closeUpDropDownControl(IDropDown *dropDown) = 0;
    virtual void setPluginImage(IPlugin *plugin, void *image) = 0;
    virtual void setPluginCaption(IPlugin *plugin, const QString &caption) = 0;

    virtual QMap<QString, IDropDown *> &getDropDowns() = 0;
    virtual QMap<QString, ICommand *> &getCommands() = 0;
    virtual QMap<QString, IEditCombobox *> &getEditComboboxes() = 0;
    virtual QMap<QString, IStatic *> &getStatics() = 0;
    virtual QMap<QString, IProgress *> &getProgresses() = 0;
    virtual QMap<QString, ITool *> &getTools() = 0;
    virtual QMap<QString, IStatusBar *> &getStatusBars() = 0;
    virtual QMap<QString, IDockWindow *> &getDockWindows() = 0;
    virtual QMap<QString, IContentsView *> &getContentsViews() = 0;
    virtual QMap<QString, IRibbonPageCategory *> &getRibbonPageCategories() = 0;
    virtual QMap<QString, IRibbonPage *> &getRibbonPages() = 0;
    virtual QMap<QString, IRibbonPageGroup *> &getRibbonPageGroups() = 0;
    virtual QMap<QString, ICheckBox *> &getCheckBoxes() = 0;
    virtual QMap<QString, IGallery *> &getGalleries() = 0;
    virtual QMap<QString, IWelcomeScreen *> &getWelcomeScreens() = 0;
    virtual QMap<QString, ISubItem *> &getSubItems() = 0;
    virtual QMap<QString, IFunction *> &getFunctions() = 0;
    virtual QMap<QString, IQuickAccessToolBar *> &getQuickAccessToolBars() = 0;
    virtual QMap<QString, IApplicationMenu *> &getApplicationMenus() = 0;
    virtual QMap<QString, IRightTopToolBar *> &getRightTopToolBars() = 0;
    virtual QMap<QString, IFloatingHost *> &getFloatingHosts() = 0;
    virtual QStringList &getAssemblyArray() = 0;
    virtual QStringList &getPackageArray() = 0;

    PluginLoadedEvent PluginLoaded;
    PluginUnLoadedEvent PluginUnLoaded;
    ContentsViewChangedEvent ContentsViewChanged;
    DockWindowChangedEvent DockWindowChanged;
    ContentsViewClosingEvent ContentsViewClosing;
    ContentsViewClosedEvent ContentsViewClosed;
    DockWindowRequestEvent CreateDockWindowRequested;
    DockWindowRequestEvent DestroyDockWindowRequested;
    DockWindowRequestEvent ActivateDockWindowRequested;
    ContentsViewCreateRequestEvent CreateContentsViewRequested;
    ContentsViewRequestEvent ActivateContentsViewRequested;
    ContentsViewRequestEvent CloseContentsViewRequested;
    ContentsViewSetTextRequestEvent SetContentsViewTextRequested;
    ContentsViewGetTextRequestEvent GetContentsViewTextRequested;
    CommandClickedEvent CommandClicked;
    CanPluginEnableEvent CanPluginEnable;
    SelectedRibbonPageChangedEvent SelectedRibbonPageChanged;

    PluginAddedEvent PluginAdded;
    PluginRemovedEvent PluginRemoved;
    PluginVisibleChangedEvent PluginVisibleChanged;
    PluginEnableChangedEvent PluginEnableChanged;
};

} // namespace PluginEngine
} // namespace RF

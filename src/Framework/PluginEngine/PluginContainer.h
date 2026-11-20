#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include "PEGlobals.h"
#include "IPluginContainer.h"
#include "PluginCollection.h"
#include "ICommand.h"
#include "ITool.h"
#include "IEditCombobox.h"
#include "ICheckBox.h"
#include "IGallery.h"
#include "IStatic.h"
#include "IProgress.h"
#include "IDropDown.h"
#include "IDockWindow.h"
#include "IStatusBar.h"
#include "IRibbonPageCategory.h"
#include "IRibbonPage.h"
#include "IRibbonPageGroup.h"
#include "IWelcomeScreen.h"
#include "ISubItem.h"
#include "IFunction.h"
#include "IConnect.h"
#include "IQuickAccessToolBar.h"
#include "IApplicationMenu.h"
#include "IRightTopToolBar.h"
#include "IFloatingHost.h"

namespace RF {
namespace PluginEngine {

class PLUGIN_ENGINE_EXPORT PluginContainer : public IPluginContainer {
private:
    QMap<QString, IDropDown*> m_dropDowns;
    QMap<QString, IConnect*> m_connects;
    QMap<QString, ICommand*> m_commands;
    QMap<QString, IEditCombobox*> m_editComboboxes;
    QMap<QString, ICheckBox*> m_checkBoxes;
    QMap<QString, IStatic*> m_statics;
    QMap<QString, IProgress*> m_progresses;
    QMap<QString, IGallery*> m_galleries;
    QMap<QString, IDockWindow*> m_dockWindows;
    QMap<QString, IContentsView*> m_contentsViews;
    QMap<QString, ITool*> m_tools;
    QMap<QString, IWelcomeScreen*> m_welcomeScreens;
    QMap<QString, IRibbonPageCategory*> m_ribbonPageCategories;
    QMap<QString, IRibbonPage*> m_ribbonPages;
    QMap<QString, IRibbonPageGroup*> m_ribbonPageGroups;
    QMap<QString, IStatusBar*> m_statusBars;
    QMap<QString, ISubItem*> m_subItems;
    QMap<QString, IFunction*> m_functions;
    QMap<QString, IQuickAccessToolBar*> m_quickAccessToolBars;
    QMap<QString, IApplicationMenu*> m_applicationMenus;
    QMap<QString, IRightTopToolBar*> m_rightTopToolBars;
    QMap<QString, IFloatingHost*> m_floatingHosts;
    QStringList m_assemblyArray;
    QStringList m_packageArray;
    QMap<QString, void *> m_objects;
    QMap<QString, const std::type_info *> m_objectTypes;
    QMap<QString, IPlugin *> m_objectOwners;

public:
    PluginContainer();
    virtual ~PluginContainer();

    void pluginVisible(IPlugin* plugin, bool visible) override;
    void pluginVisible(const QString& pluginTypeName, bool visible) override;
    bool pluginIsVisible(IPlugin* plugin) override;
    void pluginEnable(IPlugin* plugin, bool enable) override;
    void pluginEnable(const QString& pluginTypeName, bool enable) override;
    bool pluginIsEnable(IPlugin* plugin) override;
    void publishObject(const QString &name, void *object, const std::type_info &type,
                       IPlugin *owner = nullptr) override;
    void unpublishObject(const QString &name, void *object) override;
    void unpublishObjectsOwnedBy(IPlugin *owner) override;
    void *object(const QString &name) const override;
    const std::type_info *objectType(const QString &name) const override;

    void selectPage(IRibbonPage* ribbonPage) override;
    void setCommandCheckState(ICommand* command, bool checkState) override;
    void setCheckBoxState(ICheckBox* checkBox, int newCheckState) override;

    IDockWindow* createDockWindow(const QString& typeName) override;
    void destroyDockWindow(IDockWindow* dockWindow) override;
    void activeDockWindow(IDockWindow* dockWindow) override;

    IContentsView* createContentsView(const QString& typeName, const QString& key) override;
    IContentsView* createContentsView(const QString& typeName, const QString& key, const QString& caption) override;
    void activeContentsView(IContentsView* contentsView) override;
    void setContentsViewText(IContentsView* contentsView, const QString& text) override;
    QString getContentsViewText(IContentsView* contentsView) override;
    void closeContentsView(IContentsView* contentsView) override;

    void setStaticCaption(IStatic* staticPlugin, const QString& caption) override;
    void setProgressValue(IProgress* progress, int value) override;
    void setGalleryItemCheckState(void* galleryItem, bool checkState) override;
    void performStepProgress(IProgress* progress) override;
    void incrementProgress(IProgress* progress, int value) override;

    void setEditComboboxText(IEditCombobox* editCombobox, const QString& text) override;
    void setEditComboboxValue(IEditCombobox* editCombobox, void* value) override;
    int addComboBoxItem(IEditCombobox* editCombobox, void* comboBoxItem) override;
    void removeComboBoxItem(IEditCombobox* editCombobox, void* value) override;
    void clearComboBoxItem(IEditCombobox* editCombobox) override;

    void closeUpDropDownControl(IDropDown* dropDown) override;
    void setPluginImage(IPlugin* plugin, void* image) override;
    void setPluginCaption(IPlugin* plugin, const QString& caption) override;

    QMap<QString, IDropDown*>& getDropDowns() override { return m_dropDowns; }
    QMap<QString, ICommand*>& getCommands() override { return m_commands; }
    QMap<QString, IEditCombobox*>& getEditComboboxes() override { return m_editComboboxes; }
    QMap<QString, IStatic*>& getStatics() override { return m_statics; }
    QMap<QString, IProgress*>& getProgresses() override { return m_progresses; }
    QMap<QString, ITool*>& getTools() override { return m_tools; }
    QMap<QString, IStatusBar*>& getStatusBars() override { return m_statusBars; }
    QMap<QString, IDockWindow*>& getDockWindows() override { return m_dockWindows; }
    QMap<QString, IContentsView*>& getContentsViews() override { return m_contentsViews; }
    QMap<QString, IRibbonPageCategory*>& getRibbonPageCategories() override { return m_ribbonPageCategories; }
    QMap<QString, IRibbonPage*>& getRibbonPages() override { return m_ribbonPages; }
    QMap<QString, IRibbonPageGroup*>& getRibbonPageGroups() override { return m_ribbonPageGroups; }
    QMap<QString, ICheckBox*>& getCheckBoxes() override { return m_checkBoxes; }
    QMap<QString, IGallery*>& getGalleries() override { return m_galleries; }
    QMap<QString, IWelcomeScreen*>& getWelcomeScreens() override { return m_welcomeScreens; }
    QMap<QString, ISubItem*>& getSubItems() override { return m_subItems; }
    QMap<QString, IFunction*>& getFunctions() override { return m_functions; }
    QMap<QString, IQuickAccessToolBar*>& getQuickAccessToolBars() override { return m_quickAccessToolBars; }
    QMap<QString, IApplicationMenu*>& getApplicationMenus() override { return m_applicationMenus; }
    QMap<QString, IRightTopToolBar*>& getRightTopToolBars() override { return m_rightTopToolBars; }
    QMap<QString, IFloatingHost*>& getFloatingHosts() override { return m_floatingHosts; }
    QStringList& getAssemblyArray() override { return m_assemblyArray; }
    QStringList& getPackageArray() override { return m_packageArray; }
};

} // namespace PluginEngine
} // namespace RF

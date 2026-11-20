#pragma once
#include "LayoutConfig.h"
#include <IApplication.h>
#include <IServicePublisher.h>
#include <QSet>
class QAction;
class QActionGroup;
class QComboBox;
class QDir;
class QMainWindow;
class QWidget;
class SARibbonBar;
class SARibbonButtonGroupWidget;
class SARibbonCategory;
class SARibbonContextCategory;
class SARibbonPanel;
namespace ads {
class CDockAreaWidget;
class CDockManager;
class CDockWidget;
} // namespace ads
namespace RF::PluginEngine {
class IChoiceCommand;
class ICommand;
class IPlugin;
} // namespace RF::PluginEngine
namespace View3DApplication {
class PluginUiHost final {
  public:
    PluginUiHost(QMainWindow &, RF::PluginEngine::IApplication &,
                 const QSet<QString> &);
    ~PluginUiHost();
    bool buildRibbon(SARibbonBar *, const QList<View3D::RibbonPageLayout> &,
                     const QList<View3D::RibbonGroupLayout::Item> &rightButtons,
                     const QDir &, QStringList *warnings);
    bool buildContentsViews(ads::CDockManager *,
                            const QList<View3D::ContentsViewLayoutEntry> &, const QDir &,
                            QStringList *warnings);
    bool buildDockWindows(ads::CDockManager *, const QList<View3D::DockLayoutEntry> &,
                          const QDir &, QStringList *warnings);
    void setContextVisible(const QString &contextId);
    const QHash<QString, QAction *> &actions() const {
        return m_actions;
    }
    QComboBox *choice(const QString &name) const {
        return m_choices.value(name, nullptr);
    }
    RF::PluginEngine::IPlugin *contentsView(const QString &name) const {
        return m_contentsViews.value(name, nullptr);
    }
    RF::PluginEngine::IPlugin *dockWindow(const QString &name) const {
        return m_dockContents.value(name, nullptr);
    }
    ads::CDockWidget *dockWidget(const QString &name) const {
        return m_dockWidgets.value(name, nullptr);
    }
    ads::CDockWidget *contentsDockWidget(const QString &name) const {
        return m_contentsDockWidgets.value(name, nullptr);
    }
    void refreshCommandStates();
    void exposeUiObjects();

  private:
    bool buildPageContents(SARibbonCategory *, const View3D::RibbonPageLayout &, const QDir &,
                           QStringList *warnings);
    bool buildRightButtons(SARibbonButtonGroupWidget *,
                           const QList<View3D::RibbonGroupLayout::Item> &, const QDir &,
                           QStringList *warnings);
    RF::PluginEngine::ICommand *
    createCommandObject(const View3D::RibbonGroupLayout::Item &, QStringList *warnings);
    QAction *createCommandAction(const View3D::RibbonGroupLayout::Item &, const QDir &,
                                 QStringList *warnings);
    QAction *createDropdown(SARibbonPanel *, const View3D::RibbonGroupLayout::Item &,
                            const QDir &, QStringList *warnings);
    void syncChoiceItems(const QString &name, RF::PluginEngine::IChoiceCommand *choice);
    QMainWindow &m_window;
    RF::PluginEngine::IApplication &m_application;
    const QSet<QString> &m_loadedPluginIds;
    QHash<QString, QAction *> m_actions;
    QHash<QString, QComboBox *> m_choices;
    QHash<QString, QList<View3D::RibbonGroupLayout::Item::Option>> m_choiceOptions;
    // 下拉当前的内容签名（分组+value+caption）：变了才重建 QComboBox 条目。
    QHash<QString, QString> m_choiceGroups;
    QHash<QString, QActionGroup *> m_groups;
    QHash<QString, bool> m_actionConfiguredEnabled;
    QHash<QString, QString> m_actionPluginIds;
    QHash<QString, RF::PluginEngine::IPlugin *> m_contentsViews;
    QHash<QString, RF::PluginEngine::IPlugin *> m_dockContents;
    QList<RF::PluginEngine::PublishedService> m_publishedObjects;
    QHash<QString, ads::CDockWidget *> m_dockWidgets;
    QHash<QString, ads::CDockWidget *> m_contentsDockWidgets;
    QSet<QString> m_dockToggleCommands;
    SARibbonBar *m_ribbon = nullptr;
    QHash<QString, SARibbonContextCategory *> m_contextCategories;
};
} // namespace View3DApplication

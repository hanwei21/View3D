#pragma once

#include <IDockWindow.h>
#include <ISceneTreeDock.h>
#include <IServicePublisher.h>

#include <QString>

class QComboBox;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

namespace View3DPlugins {

class SceneTreeWidget;

class SceneTreeDock final : public RF::PluginEngine::IDockWindow,
                            public RF::PluginEngine::IServicePublisher,
                            public View3D::ISceneTreeDock {
  public:
    SceneTreeDock(QString name, QString caption);

    QString getCaption() const override { return m_caption; }
    RF::PluginEngine::DockingStyle getDefaultDock() const override
    {
        return RF::PluginEngine::DockingStyle::Left;
    }
    bool getInitCreate() const override { return true; }
    QString getName() const override { return m_name; }
    void onActive(bool) override {}
    void onCreate(RF::PluginEngine::IApplication*) override {}
    void onDestroy() override {}
    QWidget* getWidget() const override { return m_widget; }

    QList<RF::PluginEngine::PublishedService> publishedServices() const override;

    QTreeWidget* sceneTree() const override { return m_tree; }
    QTreeWidgetItem* sceneRootItem() const override { return m_root; }
    QLineEdit* sceneSearchEdit() const override { return m_search; }
    QComboBox* sceneTypeFilter() const override { return m_filter; }
    bool sceneTreeDropAllowed(const QPoint& position) const override;

  private:
    QString m_name;
    QString m_caption;
    QWidget* m_widget = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_filter = nullptr;
    SceneTreeWidget* m_treeWidget = nullptr;
    QTreeWidget* m_tree = nullptr;
    QTreeWidgetItem* m_root = nullptr;
};

} // namespace View3DPlugins

#pragma once

#include "SceneLayerService.h"

#include <ILayer.h>
#include <ISceneTreeMenu.h>

#include <QHash>
#include <QObject>
#include <QSet>

#include <functional>
#include <utility>

class QComboBox;
class QLineEdit;
class QMenu;
class QPoint;
class QTreeWidget;
class QTreeWidgetItem;

namespace View3D {
class IDataOpenRegistry;
class IViewportLayout;
}

namespace View3DPlugins {

// 场景树行为：层级展示、可见性勾选、选中、搜索/类型过滤、拖拽排序与右键菜单。
// 右键菜单只放通用操作；具体图层（如倾斜摄影）的定位/重载/卸载由数据插件通过
// ISceneTreeMenuContributor 注入，目录树不依赖任何数据格式。
// 图层增删改按增量更新，不重建整棵树。
class SceneTreeCoordinator final : public QObject {
  public:
    SceneTreeCoordinator(SceneLayerService& service, QTreeWidget& tree,
                         QTreeWidgetItem& root, QLineEdit* search, QComboBox* typeFilter,
                         View3D::IViewportLayout* layout,
                         View3D::ISceneTreeMenuRegistry* menuRegistry,
                         View3D::IDataOpenRegistry* dataOpenRegistry,
                         QObject* parent = nullptr);

    void refresh();
    void handleChange(const LayerChange& change);
    // 选中图层变化的一次性通知口（属性面板用）。
    void setSelectionListener(std::function<void(const QList<View3D::LayerId>&)> listener)
    {
        m_selectionListener = std::move(listener);
    }
    // 观察者应当显示的图层：当前项在前，其余选中项在后。接线时用它取初始状态，
    // 面板不必再去猜服务里的选中集合。
    QList<View3D::LayerId> observerLayerIds() const;

  private:
    void appendLayers(QTreeWidgetItem* parentItem, const View3D::LayerId& parentId);
    QTreeWidgetItem* appendLayerItem(QTreeWidgetItem* parentItem,
                                     const View3D::LayerId& layerId, int index = -1);
    void removeItemFromMap(QTreeWidgetItem* item);
    void updateItem(QTreeWidgetItem* item, const View3D::LayerId& layerId);
    void handleItemChanged(QTreeWidgetItem* item, int column);
    void handleCurrentItemChanged(QTreeWidgetItem* current);
    void handleSelectionChanged();
    void notifySelectionObservers();
    void syncOrderFromTree();
    void applyFilter();
    void filterSubtree(QTreeWidgetItem* item);
    bool filterItem(QTreeWidgetItem* item) const;
    void updateCheckStates();
    void updateCheckStatesSubtree(QTreeWidgetItem* item);
    QTreeWidgetItem* itemForLayer(const View3D::LayerId& layerId) const;
    void reportTreeCounts();
    void runVisibilityProbe();

    // 右键菜单
    void showContextMenu(const QPoint& position);
    void appendRootMenu(QMenu& menu);
    void appendGroupMenu(QMenu& menu, const View3D::LayerId& groupId);
    bool appendOpenDataAction(QMenu& menu, const View3D::LayerId& parentGroup,
                              const QString& text);
    void openData(const View3D::LayerId& parentGroup, const QString& title);
    void appendLayerMenu(QMenu& menu, const View3D::SceneTreeMenuTarget& target);
    bool appendContributorMenu(QMenu& menu, const View3D::SceneTreeMenuTarget& target);
    void appendGenericLayerActions(QMenu& menu, const View3D::LayerId& layerId);
    void createGroup(const View3D::LayerId& parentId);
    void setChildrenVisible(const View3D::LayerId& parentId, bool visible);
    void clearChildren(const View3D::LayerId& parentId);
    void setLayersVisible(const QList<View3D::LayerId>& layerIds, bool visible);
    void focusLayer(const View3D::LayerId& layerId);
    void focusAllVisible();
    bool removeLayerTree(const View3D::LayerId& layerId);
    bool removeLayerWithOwner(const View3D::LayerId& layerId);
    View3D::ISceneTreeMenuContributor* ownerOf(const View3D::LayerId& layerId) const;
    QList<View3D::LayerId> selectedLayerIds() const;

    SceneLayerService& m_service;
    QTreeWidget& m_tree;
    QTreeWidgetItem& m_root;
    QLineEdit* m_search = nullptr;
    QComboBox* m_typeFilter = nullptr;
    View3D::IViewportLayout* m_layout = nullptr;
    View3D::ISceneTreeMenuRegistry* m_menuRegistry = nullptr;
    View3D::IDataOpenRegistry* m_dataOpenRegistry = nullptr;
    QString m_lastOpenPath;
    std::function<void(const QList<View3D::LayerId>&)> m_selectionListener;
    QHash<View3D::LayerId, QTreeWidgetItem*> m_items;
    bool m_updatingTree = false;
    bool m_reported = false;
    bool m_probeScheduled = false;
};

} // namespace View3DPlugins

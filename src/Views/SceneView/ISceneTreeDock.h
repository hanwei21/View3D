#pragma once

class QComboBox;
class QLineEdit;
class QPoint;
class QTreeWidget;
class QTreeWidgetItem;

namespace View3D {

// 场景树 Dock 暴露的控件，供场景插件在扩展初始化时绑定协调器。
class ISceneTreeDock {
  public:
    virtual ~ISceneTreeDock() = default;

    virtual QTreeWidget* sceneTree() const = 0;
    virtual QTreeWidgetItem* sceneRootItem() const = 0;
    virtual QLineEdit* sceneSearchEdit() const = 0;
    virtual QComboBox* sceneTypeFilter() const = 0;
    // 拖拽落点是否在场景根内部。树内空白处等价于新建顶层项，必须返回 false。
    virtual bool sceneTreeDropAllowed(const QPoint& position) const = 0;
};

} // namespace View3D

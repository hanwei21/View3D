#include "SceneTreeDock.h"

#include <View3DObjectIds.h>

#include <QAbstractItemView>
#include <QComboBox>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPoint>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace View3DPlugins {

// 场景树只允许在场景根内部移动：空白的视图区域在 QTreeWidget 里等价于
// “新建顶层项”，拖到那里会把图层变成场景根的同级，必须拒绝。
class SceneTreeWidget final : public QTreeWidget {
  public:
    explicit SceneTreeWidget(QWidget* parent) : QTreeWidget(parent) {}

    void setSceneRootItem(QTreeWidgetItem* root) { m_root = root; }

    bool isDropPositionAllowed(const QPoint& position) const
    {
        if (!m_root)
            return false;
        QTreeWidgetItem* target = itemAt(position);
        if (!target)
            return false;
        for (QTreeWidgetItem* item = target; item; item = item->parent()) {
            if (item == m_root)
                return true;
        }
        return false;
    }

  protected:
    bool dropAllowedAt(const QPoint& position) const
    {
        if (!isDropPositionAllowed(position))
            return false;
        if (dropIndicatorPosition() == OnViewport)
            return false;
        // 落在场景根上方/下方会生成场景根的同级项，只允许落到根里面。
        if (itemAt(position) == m_root && dropIndicatorPosition() != OnItem)
            return false;
        return true;
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        QTreeWidget::dragMoveEvent(event);
        if (event->isAccepted() && !dropAllowedAt(event->position().toPoint()))
            event->ignore();
    }

    void dropEvent(QDropEvent* event) override
    {
        if (!dropAllowedAt(event->position().toPoint())) {
            event->ignore();
            return;
        }
        QTreeWidget::dropEvent(event);
    }

  private:
    QTreeWidgetItem* m_root = nullptr;
};

SceneTreeDock::SceneTreeDock(QString name, QString caption)
    : m_name(std::move(name)), m_caption(std::move(caption))
{
    if (m_name.isEmpty())
        m_name = QStringLiteral("dw.scene");
    if (m_caption.isEmpty())
        m_caption = QStringLiteral("场景");

    m_widget = new QWidget();
    auto* layout = new QVBoxLayout(m_widget);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* filters = new QHBoxLayout();
    m_search = new QLineEdit(m_widget);
    m_search->setPlaceholderText(QStringLiteral("搜索场景树……"));
    m_filter = new QComboBox(m_widget);
    m_filter->addItem(QStringLiteral("全部类型"), -1);
    m_filter->addItem(QStringLiteral("组图层"), 0);
    m_filter->addItem(QStringLiteral("图层"), 1);
    filters->addWidget(m_search, 1);
    filters->addWidget(m_filter);
    layout->addLayout(filters);

    m_treeWidget = new SceneTreeWidget(m_widget);
    m_tree = m_treeWidget;
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setHeaderLabel(QStringLiteral("场景 / 图层"));
    m_tree->setDragDropMode(QAbstractItemView::InternalMove);
    m_tree->setDefaultDropAction(Qt::MoveAction);
    // 右键菜单由场景树协调器按目标类型构建，数据插件通过注册表追加自己的条目。
    // 当前只有一个场景，整棵树只有这一个根，绑定当前三维内容视图。
    m_root = new QTreeWidgetItem({QStringLiteral("场景")});
    m_root->setToolTip(0, QStringLiteral("当前视图场景"));
    m_root->setFlags((m_root->flags() | Qt::ItemIsDropEnabled) & ~Qt::ItemIsDragEnabled);
    m_treeWidget->setSceneRootItem(m_root);
    m_tree->addTopLevelItem(m_root);
    m_root->setExpanded(true);
    layout->addWidget(m_tree, 1);
}

bool SceneTreeDock::sceneTreeDropAllowed(const QPoint& position) const
{
    return m_treeWidget && m_treeWidget->isDropPositionAllowed(position);
}

QList<RF::PluginEngine::PublishedService>
SceneTreeDock::publishedServices() const
{
    QList<RF::PluginEngine::PublishedService> result;
    result.append({View3D::ServiceIds::SceneTreeDock,
                   static_cast<View3D::ISceneTreeDock*>(const_cast<SceneTreeDock*>(this)),
                   &typeid(View3D::ISceneTreeDock)});
    return result;
}

} // namespace View3DPlugins

#include "SceneTreeCoordinator.h"

#include "SceneLayerService.h"

#include <IDataOpen.h>
#include <ILayerManager.h>
#include <IRenderScene.h>
#include <IRenderSurface.h>
#include <IViewportLayout.h>

#include <QAbstractItemModel>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QHash>
#include <QIcon>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>

#include <functional>
#include <iostream>
#include <utility>

namespace View3DPlugins {
namespace {
constexpr int LayerIdRole = Qt::UserRole + 1;

QIcon uiIcon(const QString& name)
{
    static QHash<QString, QIcon> cache;
    const auto found = cache.constFind(name);
    if (found != cache.constEnd())
        return found.value();
    const QIcon icon(QDir(QCoreApplication::applicationDirPath())
                         .filePath(QStringLiteral("resources/icons/%1.svg").arg(name)));
    cache.insert(name, icon);
    return icon;
}

QIcon layerTypeIcon(View3D::LayerType type)
{
    switch (type) {
    case View3D::LayerType::Group:
        return uiIcon(QStringLiteral("tree-group"));
    case View3D::LayerType::Model:
        return uiIcon(QStringLiteral("tree-model"));
    case View3D::LayerType::Tiled:
        return uiIcon(QStringLiteral("tree-tiled"));
    case View3D::LayerType::Imagery:
        return uiIcon(QStringLiteral("tree-imagery"));
    case View3D::LayerType::Feature:
        return uiIcon(QStringLiteral("tree-feature"));
    default:
        return uiIcon(QStringLiteral("tree-unknown"));
    }
}
} // namespace

SceneTreeCoordinator::SceneTreeCoordinator(SceneLayerService& service, QTreeWidget& tree,
                                           QTreeWidgetItem& root, QLineEdit* search,
                                           QComboBox* typeFilter,
                                           View3D::IViewportLayout* layout,
                                           View3D::ISceneTreeMenuRegistry* menuRegistry,
                                           View3D::IDataOpenRegistry* dataOpenRegistry,
                                           QObject* parent)
    : QObject(parent), m_service(service), m_tree(tree), m_root(root), m_search(search),
      m_typeFilter(typeFilter), m_layout(layout), m_menuRegistry(menuRegistry),
      m_dataOpenRegistry(dataOpenRegistry)
{
    m_root.setIcon(0, uiIcon(QStringLiteral("tree-root")));
    m_tree.setContextMenuPolicy(Qt::CustomContextMenu);
    connect(&m_tree, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& position) { showContextMenu(position); });
    connect(&m_tree, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, int column) { handleItemChanged(item, column); });
    connect(&m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current) { handleCurrentItemChanged(current); });
    connect(&m_tree, &QTreeWidget::itemSelectionChanged, this,
            [this] { handleSelectionChanged(); });
    connect(m_tree.model(), &QAbstractItemModel::rowsMoved, this,
            [this] { syncOrderFromTree(); });
    if (m_search)
        connect(m_search, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    if (m_typeFilter)
        connect(m_typeFilter, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] { applyFilter(); });
}

void SceneTreeCoordinator::refresh()
{
    if (!m_tree.model())
        return;

    QSet<View3D::LayerId> collapsed;
    std::function<void(QTreeWidgetItem*)> capture = [&](QTreeWidgetItem* item) {
        const View3D::LayerId id = item->data(0, LayerIdRole).toUuid();
        if (!id.isNull() && !item->isExpanded())
            collapsed.insert(id);
        for (int index = 0; index < item->childCount(); ++index)
            capture(item->child(index));
    };
    capture(&m_root);

    const View3D::LayerId active = m_service.layers().activeLayer();
    const QList<View3D::LayerId> selected = m_service.layers().selectedLayers();

    {
        QSignalBlocker blocker(&m_tree);
        m_updatingTree = true;
        m_items.clear();
        while (m_root.childCount() > 0)
            delete m_root.takeChild(0);

        appendLayers(&m_root, {});

        std::function<void(QTreeWidgetItem*)> restore = [&](QTreeWidgetItem* item) {
            const View3D::LayerId id = item->data(0, LayerIdRole).toUuid();
            if (!id.isNull() && collapsed.contains(id))
                item->setExpanded(false);
            for (int index = 0; index < item->childCount(); ++index)
                restore(item->child(index));
        };
        restore(&m_root);
        applyFilter();

        if (!active.isNull()) {
            if (QTreeWidgetItem* item = itemForLayer(active))
                m_tree.setCurrentItem(item);
        }
        for (const View3D::LayerId& id : selected) {
            if (QTreeWidgetItem* item = itemForLayer(id))
                item->setSelected(true);
        }
        m_updatingTree = false;
    }
    // 上面的重建全程被 QSignalBlocker 屏蔽：树自己把当前项设回活动图层时不会发信号，
    // 观察者只能靠这里补一次。面板因此不必猜 Qt 到底发没发过信号。
    notifySelectionObservers();

    reportTreeCounts();
}

void SceneTreeCoordinator::handleChange(const LayerChange& change)
{
    if (m_updatingTree) {
        QTimer::singleShot(0, this, [this] { refresh(); });
        return;
    }
    if (change.kind == LayerChangeKind::Reset) {
        refresh();
        return;
    }
    if (change.kind == LayerChangeKind::Added) {
        const View3D::LayerId parentId = m_service.layers().parentLayer(change.id);
        QTreeWidgetItem* parentItem =
            parentId.isNull() ? &m_root : itemForLayer(parentId);
        if (!parentItem) {
            refresh();
            return;
        }
        {
            QSignalBlocker blocker(&m_tree);
            m_updatingTree = true;
            QTreeWidgetItem* item = appendLayerItem(parentItem, change.id);
            if (item) {
                const bool filtering =
                    (m_search && !m_search->text().trimmed().isEmpty()) ||
                    (m_typeFilter && m_typeFilter->currentData().toInt() != -1);
                if (filtering)
                    applyFilter();
                else
                    filterSubtree(item);
            }
            if (change.id == m_service.layers().activeLayer()) {
                if (QTreeWidgetItem* current = itemForLayer(change.id))
                    m_tree.setCurrentItem(current);
            }
            m_updatingTree = false;
        }
        // 同 refresh：置当前项的动作被 QSignalBlocker 吞掉，这里补通知给属性面板。
        notifySelectionObservers();
        reportTreeCounts();
        return;
    }
    if (change.kind == LayerChangeKind::Removed) {
        if (QTreeWidgetItem* item = itemForLayer(change.id)) {
            QSignalBlocker blocker(&m_tree);
            m_updatingTree = true;
            removeItemFromMap(item);
            delete item;
            m_updatingTree = false;
            // 删掉的是当前项时 Qt 会把当前项移到邻近行，同样不发信号。
            notifySelectionObservers();
        } else {
            refresh();
        }
        return;
    }
    if (change.kind == LayerChangeKind::Updated) {
        if (QTreeWidgetItem* item = itemForLayer(change.id)) {
            QSignalBlocker blocker(&m_tree);
            m_updatingTree = true;
            updateItem(item, change.id);
            updateCheckStatesSubtree(item);
            m_updatingTree = false;
            if ((m_search && !m_search->text().trimmed().isEmpty()) ||
                (m_typeFilter && m_typeFilter->currentData().toInt() != -1))
                applyFilter();
        } else {
            refresh();
        }
    }
}

void SceneTreeCoordinator::appendLayers(QTreeWidgetItem* parentItem,
                                        const View3D::LayerId& parentId)
{
    const QList<View3D::LayerId> ids = parentId.isNull()
        ? m_service.layers().rootLayers()
        : m_service.layers().childLayers(parentId);
    for (const View3D::LayerId& id : ids)
        appendLayerItem(parentItem, id);
}

QTreeWidgetItem* SceneTreeCoordinator::appendLayerItem(
    QTreeWidgetItem* parentItem, const View3D::LayerId& layerId, int index)
{
    const auto layer = m_service.layers().layer(layerId);
    if (!layer)
        return nullptr;
    auto* item = new QTreeWidgetItem();
    item->setText(0, layer->name());
    item->setIcon(0, layerTypeIcon(layer->type()));
    if (index < 0 || index >= parentItem->childCount())
        parentItem->addChild(item);
    else
        parentItem->insertChild(index, item);
    m_items.insert(layerId, item);
    item->setData(0, LayerIdRole, layerId);
    Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled |
        Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled;
    if (layer->type() == View3D::LayerType::Group)
        flags |= Qt::ItemIsDropEnabled;
    if (!layer->isLocked())
        flags |= Qt::ItemIsEditable;
    item->setFlags(flags);
    item->setExpanded(true);
    updateItem(item, layerId);
    if (layer->type() == View3D::LayerType::Group)
        appendLayers(item, layerId);
    return item;
}

void SceneTreeCoordinator::updateItem(QTreeWidgetItem* item,
                                      const View3D::LayerId& layerId)
{
    const auto layer = m_service.layers().layer(layerId);
    if (!item || !layer)
        return;
    if (item->text(0) != layer->name())
        item->setText(0, layer->name());
    const Qt::CheckState state = layer->isVisible() ? Qt::Checked : Qt::Unchecked;
    if (item->checkState(0) != state)
        item->setCheckState(0, state);
    if (layer->isLocked())
        item->setForeground(0, QBrush(QColor(140, 140, 140)));
    else
        item->setForeground(0, QBrush());
    if (layer->type() == View3D::LayerType::Group) {
        item->setToolTip(0, QStringLiteral("组图层｜%1")
                                .arg(layer->isVisible() ? QStringLiteral("可见")
                                                        : QStringLiteral("隐藏")));
    } else {
        const QVariantMap statistics = layer->statistics();
        QString loadText = QStringLiteral("就绪");
        switch (layer->loadState()) {
        case View3D::LoadState::Queued:
        case View3D::LoadState::Loading:
        case View3D::LoadState::PartiallyReady:
            loadText = QStringLiteral("加载中");
            break;
        case View3D::LoadState::Failed:
        case View3D::LoadState::SourceInvalid:
            loadText = QStringLiteral("加载失败");
            break;
        default:
            break;
        }
        item->setToolTip(
            0, QStringLiteral("图层｜%1｜%2｜顶点 %3｜%4")
                   .arg(layer->isVisible() ? QStringLiteral("可见")
                                           : QStringLiteral("隐藏"),
                        layer->isLocked() ? QStringLiteral("锁定")
                                          : QStringLiteral("未锁定"),
                        QString::number(
                            statistics.value(QStringLiteral("vertices")).toULongLong()),
                        loadText));
    }
}

void SceneTreeCoordinator::removeItemFromMap(QTreeWidgetItem* item)
{
    if (!item)
        return;
    for (int index = 0; index < item->childCount(); ++index)
        removeItemFromMap(item->child(index));
    const View3D::LayerId id = item->data(0, LayerIdRole).toUuid();
    if (!id.isNull())
        m_items.remove(id);
}

void SceneTreeCoordinator::reportTreeCounts()
{
    if (m_reported)
        return;
    int groups = 0;
    int layers = 0;
    std::function<void(QTreeWidgetItem*)> count = [&](QTreeWidgetItem* item) {
        const View3D::LayerId id = item->data(0, LayerIdRole).toUuid();
        const auto layer = m_service.layers().layer(id);
        if (!layer)
            return;
        if (layer->type() == View3D::LayerType::Group)
            ++groups;
        else
            ++layers;
        for (int child = 0; child < item->childCount(); ++child)
            count(item->child(child));
    };
    for (int index = 0; index < m_root.childCount(); ++index)
        count(m_root.child(index));
    if (layers == 0)
        return;
    m_reported = true;
    std::cout << "VIEW3D_SCENE_TREE groups=" << groups << " layers=" << layers
              << std::endl;
    if (!m_probeScheduled && !qEnvironmentVariable("VIEW3D_TREE_PROBE").isEmpty()) {
        m_probeScheduled = true;
        QTimer::singleShot(0, this, [this] { runVisibilityProbe(); });
    }
}

void SceneTreeCoordinator::handleItemChanged(QTreeWidgetItem* item, int column)
{
    if (m_updatingTree || !item || column != 0)
        return;
    const View3D::LayerId layerId = item->data(0, LayerIdRole).toUuid();
    if (layerId.isNull())
        return;
    const auto layer = m_service.layers().layer(layerId);
    if (!layer)
        return;

    const bool visible = item->checkState(0) == Qt::Checked;
    if (visible != layer->isVisible())
        m_service.setLayerVisible(layerId, visible);

    if (item->text(0) != layer->name()) {
        if (layer->isLocked()) {
            QSignalBlocker blocker(&m_tree);
            item->setText(0, layer->name());
        } else {
            layer->setName(item->text(0));
        }
    }
}

void SceneTreeCoordinator::handleCurrentItemChanged(QTreeWidgetItem* current)
{
    if (m_updatingTree)
        return;
    const View3D::LayerId layerId =
        current ? current->data(0, LayerIdRole).toUuid() : View3D::LayerId {};
    if (layerId.isNull() || !m_service.layers().layer(layerId)) {
        // 落在场景根或空处：服务侧的选中状态由 itemSelectionChanged 统一清理，
        // 这里只让面板跟着清空，避免出现「树里没有当前项、面板还在编辑旧图层」。
        notifySelectionObservers();
        return;
    }
    m_service.layers().setActiveLayer(layerId);
    m_service.layers().setSelectedLayers({layerId});
    notifySelectionObservers();
}

void SceneTreeCoordinator::handleSelectionChanged()
{
    if (m_updatingTree)
        return;
    QList<View3D::LayerId> layerIds;
    for (QTreeWidgetItem* item : m_tree.selectedItems()) {
        const View3D::LayerId layerId = item->data(0, LayerIdRole).toUuid();
        if (!layerId.isNull() && !layerIds.contains(layerId))
            layerIds.append(layerId);
    }
    if (layerIds.isEmpty()) {
        m_service.layers().setActiveLayer({});
        m_service.layers().setSelectedLayers({});
    } else {
        m_service.layers().setActiveLayer(layerIds.first());
        m_service.layers().setSelectedLayers(layerIds);
    }
    notifySelectionObservers();
}

// 观察者要跟的是「当前项」：setCurrentItem 用的是 Select 而不是 ClearAndSelect，
// 遗留多选时 selectedLayerIds().first() 会停在旧图层上。
QList<View3D::LayerId> SceneTreeCoordinator::observerLayerIds() const
{
    QList<View3D::LayerId> layerIds;
    if (QTreeWidgetItem* current = m_tree.currentItem()) {
        const View3D::LayerId layerId = current->data(0, LayerIdRole).toUuid();
        if (!layerId.isNull() && m_service.layers().layer(layerId))
            layerIds.append(layerId);
    }
    for (const View3D::LayerId& layerId : selectedLayerIds()) {
        if (!layerIds.contains(layerId))
            layerIds.append(layerId);
    }
    return layerIds;
}

// 属性面板等观察者由扩展接线时注入；目录树本身不认识任何具体图层类型。
void SceneTreeCoordinator::notifySelectionObservers()
{
    if (m_selectionListener)
        m_selectionListener(observerLayerIds());
}

void SceneTreeCoordinator::syncOrderFromTree()
{
    if (m_updatingTree)
        return;
    // 任何图层都不能成为场景根的顶层兄弟；若出现游离顶层项，按图层模型重建。
    for (int index = 0; index < m_tree.topLevelItemCount(); ++index) {
        if (m_tree.topLevelItem(index) != &m_root) {
            QTimer::singleShot(0, this, [this] { refresh(); });
            return;
        }
    }
    bool valid = true;
    std::function<void(QTreeWidgetItem*, const View3D::LayerId&)> apply =
        [&](QTreeWidgetItem* parentItem, const View3D::LayerId& parentId) {
            int index = 0;
            for (int childIndex = 0; childIndex < parentItem->childCount(); ++childIndex) {
                QTreeWidgetItem* item = parentItem->child(childIndex);
                const View3D::LayerId layerId = item->data(0, LayerIdRole).toUuid();
                if (layerId.isNull())
                    continue;
                const auto layer = m_service.layers().layer(layerId);
                if (!layer)
                    continue;
                if (!m_service.layers().moveLayer(layerId, parentId, index))
                    valid = false;
                ++index;
                if (layer->type() == View3D::LayerType::Group)
                    apply(item, layerId);
            }
        };
    apply(&m_root, {});
    if (!valid)
        QTimer::singleShot(0, this, [this] { refresh(); });
}

void SceneTreeCoordinator::applyFilter()
{
    filterSubtree(&m_root);
}

void SceneTreeCoordinator::filterSubtree(QTreeWidgetItem* item)
{
    if (!item)
        return;
    bool anyChildVisible = false;
    for (int index = 0; index < item->childCount(); ++index) {
        filterSubtree(item->child(index));
        anyChildVisible = anyChildVisible || !item->child(index)->isHidden();
    }
    const bool match = filterItem(item);
    item->setHidden(!match && !anyChildVisible);
}

bool SceneTreeCoordinator::filterItem(QTreeWidgetItem* item) const
{
    if (!item)
        return false;
    int typeId = -1;
    const View3D::LayerId layerId = item->data(0, LayerIdRole).toUuid();
    if (!layerId.isNull()) {
        const auto layer = m_service.layers().layer(layerId);
        if (layer)
            typeId = layer->type() == View3D::LayerType::Group
                ? 0
                : layer->type() == View3D::LayerType::Model ? 1 : 2;
    }
    if (m_typeFilter && m_typeFilter->currentData().toInt() != -1 &&
        m_typeFilter->currentData().toInt() != typeId)
        return false;
    if (m_search && !m_search->text().trimmed().isEmpty()) {
        const QString needle = m_search->text().trimmed();
        if (!item->text(0).contains(needle, Qt::CaseInsensitive))
            return false;
    }
    return true;
}

void SceneTreeCoordinator::updateCheckStatesSubtree(QTreeWidgetItem* item)
{
    if (!item)
        return;
    const View3D::LayerId layerId = item->data(0, LayerIdRole).toUuid();
    if (!layerId.isNull()) {
        if (const auto layer = m_service.layers().layer(layerId)) {
            const Qt::CheckState state =
                layer->isVisible() ? Qt::Checked : Qt::Unchecked;
            if (item->checkState(0) != state)
                item->setCheckState(0, state);
        }
    }
    for (int index = 0; index < item->childCount(); ++index)
        updateCheckStatesSubtree(item->child(index));
}

void SceneTreeCoordinator::updateCheckStates()
{
    QSignalBlocker blocker(&m_tree);
    std::function<void(QTreeWidgetItem*)> update = [&](QTreeWidgetItem* item) {
        const View3D::LayerId layerId = item->data(0, LayerIdRole).toUuid();
        if (!layerId.isNull()) {
            if (const auto layer = m_service.layers().layer(layerId)) {
                const Qt::CheckState state =
                    layer->isVisible() ? Qt::Checked : Qt::Unchecked;
                if (item->checkState(0) != state)
                    item->setCheckState(0, state);
            }
        }
        for (int index = 0; index < item->childCount(); ++index)
            update(item->child(index));
    };
    update(&m_root);
}

void SceneTreeCoordinator::runVisibilityProbe()
{
    QTreeWidgetItem* target = nullptr;
    for (int index = 0; index < m_root.childCount() && !target; ++index) {
        QTreeWidgetItem* groupItem = m_root.child(index);
        const View3D::LayerId groupId = groupItem->data(0, LayerIdRole).toUuid();
        const auto group = m_service.layers().layer(groupId);
        if (!group || group->type() != View3D::LayerType::Group)
            continue;
        for (int childIndex = 0; childIndex < groupItem->childCount(); ++childIndex) {
            QTreeWidgetItem* item = groupItem->child(childIndex);
            const auto layer = m_service.layers().layer(item->data(0, LayerIdRole).toUuid());
            if (layer && layer->type() != View3D::LayerType::Group) {
                target = item;
                break;
            }
        }
    }
    if (!target) {
        std::cout << "VIEW3D_TREE_PROBE result=no_layer" << std::endl;
        return;
    }

    const View3D::LayerId layerId = target->data(0, LayerIdRole).toUuid();
    const QString renderKey = m_service.renderKey(layerId);
    View3D::IRenderScene* scene = m_service.scene();
    if (renderKey.isEmpty() || !scene) {
        std::cout << "VIEW3D_TREE_PROBE result=no_scene" << std::endl;
        return;
    }

    const bool initial = scene->isModelLayerVisible(renderKey);
    target->setCheckState(0, Qt::Unchecked);
    const bool hidden = scene->isModelLayerVisible(renderKey);
    target->setCheckState(0, Qt::Checked);
    const bool restored = scene->isModelLayerVisible(renderKey);
    std::cout << "VIEW3D_TREE_PROBE initial=" << initial << " hidden=" << hidden
              << " restored=" << restored << " key=" << renderKey.toStdString()
              << std::endl;
}

void SceneTreeCoordinator::showContextMenu(const QPoint& position)
{
    QTreeWidgetItem* item = m_tree.itemAt(position);
    View3D::SceneTreeMenuTarget target;
    if (!item || item == &m_root) {
        target.kind = View3D::SceneTreeMenuTargetKind::SceneRoot;
    } else {
        const View3D::LayerId layerId = item->data(0, LayerIdRole).toUuid();
        const auto layer = m_service.layers().layer(layerId);
        if (!layer) {
            target.kind = View3D::SceneTreeMenuTargetKind::SceneRoot;
        } else if (layer->type() == View3D::LayerType::Group) {
            target.kind = View3D::SceneTreeMenuTargetKind::Group;
            target.primary = layerId;
            target.layers = {layerId};
        } else {
            // 右键未选中的项时先切到该项，否则菜单会作用在别的选中图层上。
            if (!item->isSelected())
                m_tree.setCurrentItem(item);
            target.kind = View3D::SceneTreeMenuTargetKind::Layer;
            target.primary = layerId;
            target.layers = selectedLayerIds();
            if (!target.layers.contains(layerId))
                target.layers = {layerId};
        }
    }

    QMenu menu(&m_tree);
    switch (target.kind) {
    case View3D::SceneTreeMenuTargetKind::SceneRoot:
        appendRootMenu(menu);
        break;
    case View3D::SceneTreeMenuTargetKind::Group:
        appendGroupMenu(menu, target.primary);
        break;
    case View3D::SceneTreeMenuTargetKind::Layer:
        appendLayerMenu(menu, target);
        break;
    }
    menu.exec(m_tree.viewport()->mapToGlobal(position));
}

void SceneTreeCoordinator::appendRootMenu(QMenu& menu)
{
    if (appendOpenDataAction(menu, View3D::LayerId {}, QStringLiteral("打开数据…")))
        menu.addSeparator();

    QAction* addGroup = menu.addAction(QStringLiteral("新建图层组"));
    addGroup->setIcon(uiIcon(QStringLiteral("action-add-group")));
    connect(addGroup, &QAction::triggered, this,
            [this] { createGroup(View3D::LayerId {}); });

    const QList<View3D::LayerId> roots = m_service.layers().rootLayers();
    const bool hasLayers = !roots.isEmpty();
    menu.addSeparator();
    QAction* showAll = menu.addAction(QStringLiteral("全部显示"));
    showAll->setIcon(uiIcon(QStringLiteral("action-show")));
    connect(showAll, &QAction::triggered, this,
            [this] { setChildrenVisible(View3D::LayerId {}, true); });
    showAll->setEnabled(hasLayers);
    QAction* hideAll = menu.addAction(QStringLiteral("全部隐藏"));
    hideAll->setIcon(uiIcon(QStringLiteral("action-hide")));
    connect(hideAll, &QAction::triggered, this,
            [this] { setChildrenVisible(View3D::LayerId {}, false); });
    hideAll->setEnabled(hasLayers);
    QAction* focusAll = menu.addAction(QStringLiteral("聚焦全部可见"));
    focusAll->setIcon(uiIcon(QStringLiteral("action-focus")));
    connect(focusAll, &QAction::triggered, this, [this] { focusAllVisible(); });
    focusAll->setEnabled(hasLayers);
    menu.addSeparator();
    QAction* clearAll = menu.addAction(QStringLiteral("清空全部图层"));
    clearAll->setIcon(uiIcon(QStringLiteral("action-clear")));
    connect(clearAll, &QAction::triggered, this,
            [this] { clearChildren(View3D::LayerId {}); });
    clearAll->setEnabled(hasLayers);

    View3D::SceneTreeMenuTarget target;
    target.kind = View3D::SceneTreeMenuTargetKind::SceneRoot;
    appendContributorMenu(menu, target);
}

void SceneTreeCoordinator::appendGroupMenu(QMenu& menu, const View3D::LayerId& groupId)
{
    QAction* addGroup = menu.addAction(QStringLiteral("新建子图层组"));
    addGroup->setIcon(uiIcon(QStringLiteral("action-add-group")));
    connect(addGroup, &QAction::triggered, this, [this, groupId] { createGroup(groupId); });
    appendOpenDataAction(menu, groupId, QStringLiteral("打开数据到本组…"));

    const bool hasChildren = !m_service.layers().childLayers(groupId).isEmpty();
    menu.addSeparator();
    QAction* showGroup = menu.addAction(QStringLiteral("组内全部显示"));
    showGroup->setIcon(uiIcon(QStringLiteral("action-show")));
    connect(showGroup, &QAction::triggered, this,
            [this, groupId] { setChildrenVisible(groupId, true); });
    showGroup->setEnabled(hasChildren);
    QAction* hideGroup = menu.addAction(QStringLiteral("组内全部隐藏"));
    hideGroup->setIcon(uiIcon(QStringLiteral("action-hide")));
    connect(hideGroup, &QAction::triggered, this,
            [this, groupId] { setChildrenVisible(groupId, false); });
    hideGroup->setEnabled(hasChildren);
    menu.addSeparator();
    QAction* clearGroup = menu.addAction(QStringLiteral("清空组内图层"));
    clearGroup->setIcon(uiIcon(QStringLiteral("action-clear")));
    connect(clearGroup, &QAction::triggered, this,
            [this, groupId] { clearChildren(groupId); });
    clearGroup->setEnabled(hasChildren);
    QAction* removeGroup = menu.addAction(QStringLiteral("删除图层组"));
    removeGroup->setIcon(uiIcon(QStringLiteral("action-delete")));
    connect(removeGroup, &QAction::triggered, this,
            [this, groupId] { removeLayerTree(groupId); });

    View3D::SceneTreeMenuTarget target;
    target.kind = View3D::SceneTreeMenuTargetKind::Group;
    target.primary = groupId;
    target.layers = {groupId};
    appendContributorMenu(menu, target);
}

bool SceneTreeCoordinator::appendOpenDataAction(QMenu& menu,
                                                const View3D::LayerId& parentGroup,
                                                const QString& text)
{
    if (!m_dataOpenRegistry || m_dataOpenRegistry->dataOpeners().isEmpty())
        return false;
    QAction* action = menu.addAction(text);
    action->setIcon(uiIcon(QStringLiteral("action-open")));
    connect(action, &QAction::triggered, this, [this, parentGroup, text] {
        QString title = text;
        if (title.endsWith(QChar(0x2026)))
            title.chop(1);
        openData(parentGroup, title);
    });
    return true;
}

void SceneTreeCoordinator::openData(const View3D::LayerId& parentGroup, const QString& title)
{
    QList<View3D::IDataOpener*> openers;
    QStringList filters;
    if (m_dataOpenRegistry) {
        for (View3D::IDataOpener* opener : m_dataOpenRegistry->dataOpeners()) {
            if (!opener)
                continue;
            const QString filter = opener->fileFilter().trimmed();
            if (filter.isEmpty())
                continue;
            openers.append(opener);
            filters.append(filter);
        }
    }
    if (openers.isEmpty())
        return;
    // 没有“所有文件”以外更合适的兜底：用户选它时仍交给第一个数据插件判定。
    filters.append(QStringLiteral("所有文件 (*)"));
    QString selectedFilter = filters.first();
    const QString start = m_lastOpenPath.isEmpty() ? QDir::homePath() : m_lastOpenPath;
    const QString path = QFileDialog::getOpenFileName(
        &m_tree, title, start, filters.join(QStringLiteral(";;")), &selectedFilter);
    if (path.isEmpty())
        return;
    m_lastOpenPath = path;
    const int index = filters.indexOf(selectedFilter);
    // 选了“所有文件”时按登记顺序逐个尝试，让数据插件自己判定能否处理。
    const QList<View3D::IDataOpener*> candidates =
        index >= 0 && index < openers.size() ? QList<View3D::IDataOpener*> {openers.at(index)}
                                             : openers;
    QString error;
    bool opened = false;
    for (View3D::IDataOpener* opener : candidates) {
        if (opener->openData(path, parentGroup, &error)) {
            opened = true;
            break;
        }
    }
    if (!opened) {
        const QString message = error.isEmpty() ? QStringLiteral("无法打开数据") : error;
        std::cerr << "VIEW3D_DATA_OPEN_FAILED reason=" << message.toStdString() << std::endl;
        QMessageBox::warning(&m_tree, title, message);
    }
}

bool SceneTreeCoordinator::appendContributorMenu(QMenu& menu,
                                                 const View3D::SceneTreeMenuTarget& target)
{
    if (!m_menuRegistry)
        return false;
    QList<View3D::SceneTreeMenuAction> actions;
    for (View3D::ISceneTreeMenuContributor* contributor :
         m_menuRegistry->sceneTreeMenuContributors()) {
        if (contributor)
            contributor->appendSceneTreeMenu(target, &actions);
    }
    bool first = true;
    for (const View3D::SceneTreeMenuAction& action : std::as_const(actions)) {
        if (action.text.isEmpty() || !action.triggered)
            continue;
        if (action.separatorBefore || (first && !menu.isEmpty()))
            menu.addSeparator();
        QAction* entry = menu.addAction(action.text);
        entry->setEnabled(action.enabled);
        if (!action.icon.isEmpty())
            entry->setIcon(uiIcon(action.icon));
        connect(entry, &QAction::triggered, this,
                [callback = action.triggered] { callback(); });
        first = false;
    }
    return !first;
}

void SceneTreeCoordinator::appendLayerMenu(QMenu& menu,
                                           const View3D::SceneTreeMenuTarget& target)
{
    if (target.layers.size() > 1) {
        const QList<View3D::LayerId> layerIds = target.layers;
        QAction* batchShow = menu.addAction(QStringLiteral("批量显示"));
        batchShow->setIcon(uiIcon(QStringLiteral("action-show")));
        connect(batchShow, &QAction::triggered, this,
                [this, layerIds] { setLayersVisible(layerIds, true); });
        QAction* batchHide = menu.addAction(QStringLiteral("批量隐藏"));
        batchHide->setIcon(uiIcon(QStringLiteral("action-hide")));
        connect(batchHide, &QAction::triggered, this,
                [this, layerIds] { setLayersVisible(layerIds, false); });
    }
    // 数据插件拥有该图层时，定位/重载/卸载都来自插件，目录树只保留批量操作。
    if (appendContributorMenu(menu, target))
        return;
    if (target.layers.size() > 1)
        menu.addSeparator();
    appendGenericLayerActions(menu, target.primary);
}

void SceneTreeCoordinator::appendGenericLayerActions(QMenu& menu,
                                                     const View3D::LayerId& layerId)
{
    QAction* locate = menu.addAction(QStringLiteral("定位到当前图层"));
    locate->setIcon(uiIcon(QStringLiteral("action-locate")));
    locate->setEnabled(m_layout && !m_service.renderKey(layerId).isEmpty());
    connect(locate, &QAction::triggered, this, [this, layerId] { focusLayer(layerId); });
    menu.addSeparator();
    QAction* remove = menu.addAction(QStringLiteral("删除图层"));
    remove->setIcon(uiIcon(QStringLiteral("action-delete")));
    connect(remove, &QAction::triggered, this, [this, layerId] { removeLayerTree(layerId); });
}

void SceneTreeCoordinator::createGroup(const View3D::LayerId& parentId)
{
    QSet<QString> names;
    const QList<View3D::LayerId> siblings = parentId.isNull()
        ? m_service.layers().rootLayers()
        : m_service.layers().childLayers(parentId);
    for (const View3D::LayerId& siblingId : siblings) {
        const auto layer = m_service.layers().layer(siblingId);
        if (layer && layer->type() == View3D::LayerType::Group)
            names.insert(layer->name());
    }
    const QString base = QStringLiteral("新建图层组");
    QString name = base;
    for (int index = 2; names.contains(name); ++index)
        name = QStringLiteral("%1 %2").arg(base).arg(index);
    if (m_service.addGroup(name, parentId).isNull())
        std::cerr << "VIEW3D_SCENE_TREE_GROUP_FAILED" << std::endl;
}

void SceneTreeCoordinator::setChildrenVisible(const View3D::LayerId& parentId, bool visible)
{
    const QList<View3D::LayerId> children = parentId.isNull()
        ? m_service.layers().rootLayers()
        : m_service.layers().childLayers(parentId);
    setLayersVisible(children, visible);
}

void SceneTreeCoordinator::clearChildren(const View3D::LayerId& parentId)
{
    const QList<View3D::LayerId> children = parentId.isNull()
        ? m_service.layers().rootLayers()
        : m_service.layers().childLayers(parentId);
    for (const View3D::LayerId& childId : children)
        removeLayerTree(childId);
}

void SceneTreeCoordinator::setLayersVisible(const QList<View3D::LayerId>& layerIds,
                                            bool visible)
{
    for (const View3D::LayerId& layerId : layerIds) {
        if (!layerId.isNull())
            m_service.setLayerVisible(layerId, visible);
    }
}

void SceneTreeCoordinator::focusLayer(const View3D::LayerId& layerId)
{
    if (!m_layout)
        return;
    const auto layer = m_service.layers().layer(layerId);
    const QString key = m_service.renderKey(layerId);
    if (!layer || key.isEmpty())
        return;
    const bool tiled = layer->type() == View3D::LayerType::Tiled;
    const auto focusOne = [&](View3D::IRenderSurface* view) {
        if (!view)
            return;
        if (tiled)
            view->focusTileGroup(key);
        else
            view->focusModelLayer(key);
    };
    const QList<View3D::IRenderSurface*> views = m_layout->views();
    if (views.isEmpty()) {
        focusOne(m_layout->activeView());
        return;
    }
    for (View3D::IRenderSurface* view : views)
        focusOne(view);
}

void SceneTreeCoordinator::focusAllVisible()
{
    if (!m_layout)
        return;
    const QList<View3D::IRenderSurface*> views = m_layout->views();
    if (views.isEmpty()) {
        if (View3D::IRenderSurface* view = m_layout->activeView())
            view->focusAllVisible();
        return;
    }
    for (View3D::IRenderSurface* view : views) {
        if (view)
            view->focusAllVisible();
    }
}

bool SceneTreeCoordinator::removeLayerTree(const View3D::LayerId& layerId)
{
    if (!m_service.layers().layer(layerId))
        return false;
    bool allRemoved = true;
    const QList<View3D::LayerId> children = m_service.layers().childLayers(layerId);
    for (const View3D::LayerId& childId : children)
        allRemoved = removeLayerTree(childId) && allRemoved;
    if (!allRemoved)
        return false;
    return removeLayerWithOwner(layerId);
}

bool SceneTreeCoordinator::removeLayerWithOwner(const View3D::LayerId& layerId)
{
    if (View3D::ISceneTreeMenuContributor* owner = ownerOf(layerId)) {
        if (owner->removeOwnedSceneTreeLayer(layerId))
            return true;
        std::cerr << "VIEW3D_SCENE_TREE_REMOVE_FAILED layer="
                  << layerId.toString().toStdString() << std::endl;
        return false;
    }
    return m_service.removeLayer(layerId);
}

View3D::ISceneTreeMenuContributor*
SceneTreeCoordinator::ownerOf(const View3D::LayerId& layerId) const
{
    if (!m_menuRegistry)
        return nullptr;
    for (View3D::ISceneTreeMenuContributor* contributor :
         m_menuRegistry->sceneTreeMenuContributors()) {
        if (contributor && contributor->ownsSceneTreeLayer(layerId))
            return contributor;
    }
    return nullptr;
}

QList<View3D::LayerId> SceneTreeCoordinator::selectedLayerIds() const
{
    QList<View3D::LayerId> layerIds;
    for (QTreeWidgetItem* item : m_tree.selectedItems()) {
        const View3D::LayerId layerId = item->data(0, LayerIdRole).toUuid();
        if (!layerId.isNull() && !layerIds.contains(layerId))
            layerIds.append(layerId);
    }
    return layerIds;
}

QTreeWidgetItem* SceneTreeCoordinator::itemForLayer(const View3D::LayerId& layerId) const
{
    return m_items.value(layerId, nullptr);
}

} // namespace View3DPlugins

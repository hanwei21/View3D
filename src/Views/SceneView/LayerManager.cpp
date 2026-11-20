#include "LayerManager.h"

#include <utility>

namespace View3D {

bool LayerManager::addLayer(const LayerPtr& layer, const LayerId& parentGroup)
{
    if (!layer || m_layers.contains(layer->id()) ||
        (!parentGroup.isNull() && (!isGroup(parentGroup) || parentDepth(parentGroup) >= 8))) {
        return false;
    }
    m_layers.insert(layer->id(), layer);
    if (parentGroup.isNull()) {
        m_order.append(layer->id());
    } else {
        m_children[parentGroup].append(layer->id());
        m_parents.insert(layer->id(), parentGroup);
    }
    if (layer->type() == LayerType::Group) m_children.insert(layer->id(), {});
    m_active = layer->id();
    return true;
}

bool LayerManager::removeLayer(const LayerId& layerId)
{
    if (!m_layers.contains(layerId)) return false;
    QList<LayerId> removed;
    collectSubtree(layerId, removed);
    const LayerId parent = m_parents.value(layerId);
    if (parent.isNull()) m_order.removeAll(layerId);
    else m_children[parent].removeAll(layerId);
    for (auto it = removed.crbegin(); it != removed.crend(); ++it) {
        m_children.remove(*it);
        m_parents.remove(*it);
        m_layers.remove(*it);
        m_selected.removeAll(*it);
        if (m_active == *it) m_active = {};
    }
    return true;
}

ILayerManager::LayerPtr LayerManager::layer(const LayerId& layerId) const
{
    return m_layers.value(layerId);
}

QList<LayerId> LayerManager::childLayers(const LayerId& groupId) const
{
    return isGroup(groupId) ? m_children.value(groupId) : QList<LayerId> {};
}

LayerId LayerManager::parentLayer(const LayerId& layerId) const
{
    return m_layers.contains(layerId) ? m_parents.value(layerId) : LayerId {};
}

bool LayerManager::moveLayer(const LayerId& layerId, const LayerId& newParentGroup, int index)
{
    if (!m_layers.contains(layerId) || layerId == newParentGroup ||
        (!newParentGroup.isNull() && !isGroup(newParentGroup)) ||
        (!newParentGroup.isNull() && isDescendant(newParentGroup, layerId)) ||
        parentDepth(newParentGroup) + subtreeDepth(layerId) > 8) {
        return false;
    }
    const LayerId oldParent = m_parents.value(layerId);
    QList<LayerId>& oldOrder = oldParent.isNull() ? m_order : m_children[oldParent];
    oldOrder.removeAll(layerId);
    QList<LayerId>& newOrder = newParentGroup.isNull() ? m_order : m_children[newParentGroup];
    newOrder.insert(qBound(0, index, newOrder.size()), layerId);
    if (newParentGroup.isNull()) m_parents.remove(layerId);
    else m_parents.insert(layerId, newParentGroup);
    return true;
}

bool LayerManager::setGroupVisible(const LayerId& groupId, bool visible)
{
    if (!isGroup(groupId)) return false;
    applyGroupState(groupId, true, visible);
    return true;
}

bool LayerManager::setGroupLocked(const LayerId& groupId, bool locked)
{
    if (!isGroup(groupId)) return false;
    applyGroupState(groupId, false, locked);
    return true;
}

void LayerManager::setActiveLayer(const LayerId& layerId)
{
    if (layerId.isNull() || m_layers.contains(layerId)) m_active = layerId;
}

void LayerManager::setSelectedLayers(const QList<LayerId>& layerIds)
{
    QList<LayerId> validated;
    validated.reserve(layerIds.size());
    for (const LayerId& layerId : layerIds) {
        if (!layerId.isNull() && m_layers.contains(layerId) && !validated.contains(layerId))
            validated.append(layerId);
    }
    m_selected = std::move(validated);
}

void LayerManager::clear()
{
    m_layers.clear();
    m_order.clear();
    m_children.clear();
    m_parents.clear();
    m_selected.clear();
    m_active = {};
}

bool LayerManager::isGroup(const LayerId& layerId) const
{
    const LayerPtr value = m_layers.value(layerId);
    return value && value->type() == LayerType::Group;
}

bool LayerManager::isDescendant(const LayerId& candidate, const LayerId& ancestor) const
{
    LayerId current = candidate;
    while (!current.isNull()) {
        if (current == ancestor) return true;
        current = m_parents.value(current);
    }
    return false;
}

int LayerManager::subtreeDepth(const LayerId& layerId) const
{
    int maximum = 1;
    for (const LayerId& child : m_children.value(layerId))
        maximum = qMax(maximum, 1 + subtreeDepth(child));
    return maximum;
}

int LayerManager::parentDepth(const LayerId& parentId) const
{
    int depth = 0;
    LayerId current = parentId;
    while (!current.isNull()) {
        ++depth;
        current = m_parents.value(current);
    }
    return depth;
}

void LayerManager::collectSubtree(const LayerId& layerId, QList<LayerId>& result) const
{
    result.append(layerId);
    for (const LayerId& child : m_children.value(layerId)) collectSubtree(child, result);
}

void LayerManager::applyGroupState(const LayerId& groupId, bool visibleState, bool value)
{
    const LayerPtr group = m_layers.value(groupId);
    if (visibleState) group->setVisible(value);
    else group->setLocked(value);
    for (const LayerId& childId : m_children.value(groupId)) {
        const LayerPtr child = m_layers.value(childId);
        if (visibleState) child->setVisible(value);
        else child->setLocked(value);
        if (child->type() == LayerType::Group) applyGroupState(childId, visibleState, value);
    }
}

} // namespace View3D

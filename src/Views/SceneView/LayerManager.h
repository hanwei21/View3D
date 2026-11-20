#pragma once

#include <ILayerManager.h>

#include <QtCore/QHash>

namespace View3D {

class LayerManager final : public ILayerManager {
public:
    bool addLayer(const LayerPtr& layer, const LayerId& parentGroup = LayerId()) override;
    bool removeLayer(const LayerId& layerId) override;
    LayerPtr layer(const LayerId& layerId) const override;
    QList<LayerId> rootLayers() const override { return m_order; }
    QList<LayerId> childLayers(const LayerId& groupId) const override;
    LayerId parentLayer(const LayerId& layerId) const override;
    bool moveLayer(const LayerId& layerId, const LayerId& newParentGroup, int index) override;
    bool setGroupVisible(const LayerId& groupId, bool visible) override;
    bool setGroupLocked(const LayerId& groupId, bool locked) override;
    LayerId activeLayer() const override { return m_active; }
    void setActiveLayer(const LayerId& layerId) override;
    QList<LayerId> selectedLayers() const override { return m_selected; }
    void setSelectedLayers(const QList<LayerId>& layerIds) override;
    void clear() override;

private:
    bool isGroup(const LayerId& layerId) const;
    bool isDescendant(const LayerId& candidate, const LayerId& ancestor) const;
    int subtreeDepth(const LayerId& layerId) const;
    int parentDepth(const LayerId& parentId) const;
    void collectSubtree(const LayerId& layerId, QList<LayerId>& result) const;
    void applyGroupState(const LayerId& groupId, bool visibleState, bool value);

    QHash<LayerId, LayerPtr> m_layers;
    QList<LayerId> m_order;
    QHash<LayerId, QList<LayerId>> m_children;
    QHash<LayerId, LayerId> m_parents;
    QList<LayerId> m_selected;
    LayerId m_active;
};

} // namespace View3D

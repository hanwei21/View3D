#pragma once

#include "ILayer.h"

#include <QtCore/QList>

#include <memory>

namespace View3D {

class ILayerManager {
public:
    using LayerPtr = std::shared_ptr<ILayer>;

    virtual ~ILayerManager() = default;

    virtual bool addLayer(const LayerPtr& layer, const LayerId& parentGroup = LayerId()) = 0;
    virtual bool removeLayer(const LayerId& layerId) = 0;
    virtual LayerPtr layer(const LayerId& layerId) const = 0;
    virtual QList<LayerId> rootLayers() const = 0;
    virtual QList<LayerId> childLayers(const LayerId& groupId) const = 0;
    virtual LayerId parentLayer(const LayerId& layerId) const = 0;
    virtual bool moveLayer(const LayerId& layerId, const LayerId& newParentGroup, int index) = 0;
    virtual bool setGroupVisible(const LayerId& groupId, bool visible) = 0;
    virtual bool setGroupLocked(const LayerId& groupId, bool locked) = 0;

    virtual LayerId activeLayer() const = 0;
    virtual void setActiveLayer(const LayerId& layerId) = 0;
    virtual QList<LayerId> selectedLayers() const = 0;
    virtual void setSelectedLayers(const QList<LayerId>& layerIds) = 0;

    virtual void clear() = 0;
};

} // namespace View3D

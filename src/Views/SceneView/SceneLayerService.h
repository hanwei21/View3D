#pragma once

#include <ISceneLayerService.h>
#include <LayerManager.h>

#include <IRenderScene.h>

#include <QHash>
#include <QList>

#include <array>
#include <functional>
#include <vector>
#include <memory>

namespace View3DPlugins {

enum class LayerChangeKind { Added, Removed, Updated, Reset };

struct LayerChange {
    LayerChangeKind kind = LayerChangeKind::Reset;
    View3D::LayerId id;
};

// 单场景的图层数据源：登记层级/元数据，并同步到共享渲染场景。
class SceneLayerService final : public View3D::ISceneLayerService {
  public:
    using ChangeCallback = std::function<void(const LayerChange&)>;

    explicit SceneLayerService(View3D::IRenderScene& scene);
    ~SceneLayerService() override;

    View3D::LayerId addGroup(const QString& name,
                             const View3D::LayerId& parentGroup = {}) override;
    View3D::LayerId addModelLayer(
        const QString& name, const QString& renderKey,
        const std::shared_ptr<const View3D::ModelMeshDto>& mesh,
        const View3D::LayerId& parentGroup = {},
        const QString& sourceCrs = {}) override;
    View3D::LayerId addTileLayer(
        const QString& name, const QString& tileGroupKey,
        const View3D::LayerId& parentGroup = {},
        const QString& sourceCrs = {}) override;
    View3D::LayerId addImageryLayer(const QString& name, const QString& renderKey,
                                    const View3D::LayerId& parentGroup = {}) override;
    View3D::LayerId addFeatureLayer(const QString& name, const QString& renderKey,
                                    const View3D::LayerId& parentGroup = {}) override;
    bool removeLayer(const View3D::LayerId& layerId) override;
    void clear() override;
    bool replaceModelLayerGeometry(const View3D::LayerId& layerId,
                                   const std::shared_ptr<const View3D::ModelMeshDto>& mesh) override;

    bool setLayerVisible(const View3D::LayerId& layerId, bool visible) override;
    bool setLayerOpacity(const View3D::LayerId& layerId, float opacity) override;
    bool setLayerOffset(const View3D::LayerId& layerId,
                        const std::array<double, 3>& offset) override;
    bool setLayerVisibleDistance(const View3D::LayerId& layerId, double minimumDistance,
                                 double maximumDistance) override;
    bool setLayerStyle(const View3D::LayerId& layerId,
                       const std::array<float, 3>& tint, bool edgeLinesVisible,
                       float brightness, float contrast) override;

    View3D::LayerId layerByRenderKey(const QString& renderKey) const override;
    QString layerName(const View3D::LayerId& layerId) const override;
    QString layerSourceCrs(const View3D::LayerId& layerId) const override;
    bool setLayerReferenceFrame(const View3D::LayerId& layerId,
                                const View3D::ILayer::ReferenceFrame& frame) override;
    View3D::ILayer::ReferenceFrame layerReferenceFrame(
        const View3D::LayerId& layerId) const override;

    View3D::LayerManager& layers() noexcept { return m_layers; }
    const View3D::LayerManager& layers() const noexcept { return m_layers; }
    QString renderKey(const View3D::LayerId& layerId) const;
    View3D::IRenderScene* scene() const noexcept { return &m_scene; }

    // 目录树与属性面板都要跟随图层变化，所以回调是多播而不是独占。
    void addChangeCallback(ChangeCallback callback);

  private:
    View3D::IRenderScene& m_scene;
    View3D::LayerManager m_layers;
    QHash<View3D::LayerId, QString> m_renderKeys;
    QHash<QString, View3D::LayerId> m_layersByRenderKey;
    // 瓦片图层：图层 id -> 瓦片组 key（组内瓦片由数据插件流式注册）。
    QHash<View3D::LayerId, QString> m_tileGroups;
    QHash<QString, View3D::LayerId> m_layersByTileGroup;
    std::vector<ChangeCallback> m_changed;

    void notify(LayerChangeKind kind, const View3D::LayerId& id = {});
    void collectSubtree(const View3D::LayerId& layerId,
                        QList<View3D::LayerId>* ids) const;
    void applyGroupVisibility(const View3D::LayerId& groupId, bool visible);
    bool parentChainVisible(const View3D::LayerId& layerId) const;
    void applySceneRenderState(const QString& renderKey, View3D::LayerRenderState state);
    View3D::LayerId addGeoLayer(const QString& name, const QString& renderKey,
                                const View3D::LayerId& parentGroup, View3D::LayerType type);
};

} // namespace View3DPlugins

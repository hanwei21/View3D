#pragma once

#include "ModelMesh.h"
#include "RenderTypes.h"
#include "ViewportPickResult.h"

#include <QList>
#include <QString>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace View3D {

struct RenderBounds {
    std::array<double, 3> minimum {};
    std::array<double, 3> maximum {};
    bool valid = false;
};

// 共享场景的内容端口：一个场景被同文档的多个视口共用。
class ISceneContent {
  public:
    virtual ~ISceneContent() = default;

    virtual bool addModelLayer(const QString& layerKey,
                               std::shared_ptr<const ModelMeshDto> mesh) = 0;
    // 运行期整块替换几何（动态投影重放用）：图层键、可见性/透明度/偏移/样式等状态
    // 全部保留，只换顶点/索引并重建拾取与包围盒。渲染端不支持时返回 false。
    virtual bool updateModelLayerMesh(const QString& layerKey,
                                      std::shared_ptr<const ModelMeshDto> mesh)
    {
        static_cast<void>(layerKey);
        static_cast<void>(mesh);
        return false;
    }
    virtual bool removeModelLayer(const QString& layerKey) = 0;
    virtual void clearModelLayers() = 0;
    virtual bool setModelLayerVisible(const QString& layerKey, bool visible) = 0;
    virtual bool isModelLayerVisible(const QString& layerKey) const = 0;
    virtual bool setModelLayerOpacity(const QString& layerKey, float opacity) = 0;
    virtual float modelLayerOpacity(const QString& layerKey) const = 0;
    virtual bool setModelLayerOffset(const QString& layerKey,
                                     const std::array<double, 3>& offset) = 0;
    virtual bool modelLayerOffset(const QString& layerKey,
                                  std::array<double, 3>* offset) const = 0;
    virtual bool setModelLayerDistanceLimits(const QString& layerKey,
                                             double minimumDistance,
                                             double maximumDistance) = 0;
    virtual bool setModelLayerStyle(const QString& layerKey,
                                    const std::array<float, 3>& tint,
                                    bool edgeLinesVisible,
                                    float brightness,
                                    float contrast) = 0;
    virtual bool selectModelLayerRange(const QString& layerKey,
                                       std::uint32_t firstPrimitive,
                                       std::uint32_t primitiveCount) = 0;
    virtual QList<QString> modelLayerKeys() const = 0;
    virtual std::shared_ptr<const ModelMeshDto> modelLayerMesh(
        const QString& layerKey) const = 0;
    virtual LayerRenderState modelLayerRenderState(
        const QString& layerKey) const = 0;
    virtual void setLayerStateCallback(
        std::function<void(const QString&, LayerRenderState)> callback) = 0;
    virtual bool modelLayerBounds(const QString& layerKey, RenderBounds* bounds) const = 0;
    virtual bool modelLayerNodeBounds(const QString& layerKey, std::uint32_t nodeIndex,
                                      RenderBounds* bounds) const = 0;
    virtual std::vector<PbrMaterialState> modelLayerPbrMaterialStates(
        const QString& layerKey) const = 0;

    virtual bool addTile(const QString& tileGroupKey, const QString& tileKey,
                         std::shared_ptr<const ModelMeshDto> mesh) = 0;
    virtual bool removeTile(const QString& tileGroupKey, const QString& tileKey) = 0;
    virtual bool setTileVisible(const QString& tileGroupKey, const QString& tileKey,
                                bool visible) = 0;
    // 同一瓦片内按 PagedLOD 节点控制可见性：bit i 对应 mesh.nodes 里
    // sourceNodeIndex==i 的部分（-1 的文件自身几何跟随整块瓦片）。
    // 渲染器不支持时返回 false，整块瓦片的可见性仍然有效。
    virtual bool setTileNodeMask(const QString& tileGroupKey, const QString& tileKey,
                                 std::uint64_t nodeMask)
    {
        static_cast<void>(tileGroupKey);
        static_cast<void>(tileKey);
        static_cast<void>(nodeMask);
        return false;
    }
    virtual bool isTileVisible(const QString& tileGroupKey,
                               const QString& tileKey) const = 0;
    // 瓦片内容是否已经可以真正上屏。GPU 上传是异步的，"已登记/已转换"不等于
    // "可显示"：父级必须等子级返回 Ready 才能隐藏，否则会先闪出空洞再突然出现。
    // 不支持异步上传的渲染器可返回 Ready。
    virtual LayerRenderState tileRenderState(const QString& tileGroupKey,
                                             const QString& tileKey) const
    {
        static_cast<void>(tileGroupKey);
        static_cast<void>(tileKey);
        return LayerRenderState::Ready;
    }
    virtual bool setTileOpacity(const QString& tileGroupKey, const QString& tileKey,
                                float opacity) = 0;
    virtual bool setTileOffset(const QString& tileGroupKey, const QString& tileKey,
                               const std::array<double, 3>& offset) = 0;
    virtual float tileOpacity(const QString& tileGroupKey,
                              const QString& tileKey) const = 0;
    virtual bool tilesHaveOpacity(const QString& tileGroupKey, float opacity) const = 0;
    virtual bool tilesUseTransparency(const QString& tileGroupKey,
                                      bool transparent) const = 0;
    virtual std::uint32_t tileCount(const QString& tileGroupKey) const noexcept = 0;
    virtual bool tileGroupBounds(const QString& tileGroupKey, RenderBounds* bounds) const = 0;
    // 瓦片组的用户通道：目录树勾选/透明度/偏移走这里，
    // setTileVisible 留给 LOD 流式调度，两者互不覆盖。
    virtual bool setTileGroupVisible(const QString& tileGroupKey, bool visible) = 0;
    virtual bool setTileGroupOpacity(const QString& tileGroupKey, float opacity) = 0;
    virtual bool setTileGroupOffset(const QString& tileGroupKey,
                                    const std::array<double, 3>& offset) = 0;
    // 瓦片组的水平刚体摆放：绕 up 轴旋转 yawDegrees（右手定则，+Y 为 up）、
    // 水平各向同性缩放 horizontalScale，再加一个地理摆放平移 placementOffset。
    // 用于显示投影切换时让无法逐点重放的流式数据（倾斜摄影、3D Tiles）整体跟随
    // 网格北；拾取与包围盒必须同步生效。placementOffset 与用户通道
    // setTileGroupOffset 分开存放，切换投影不会把用户手动挪过的距离抹掉。
    // 渲染端不支持时返回 false。
    virtual bool setTileGroupPlaneTransform(const QString& tileGroupKey, double yawDegrees,
                                            double horizontalScale,
                                            const std::array<double, 3>& placementOffset = {})
    {
        static_cast<void>(tileGroupKey);
        static_cast<void>(yawDegrees);
        static_cast<void>(horizontalScale);
        static_cast<void>(placementOffset);
        return false;
    }
    virtual bool clearTileGroup(const QString& tileGroupKey) = 0;
    virtual bool setTileDebugOverlayEnabled(bool enabled) = 0;
    virtual bool tileDebugOverlayEnabled() const noexcept = 0;

    virtual bool pickRay(const std::array<double, 3>& origin,
                         const std::array<double, 3>& direction,
                         ViewportPickResult* result) const = 0;

    virtual bool highlightModelFeature(const QString& layerKey,
                                       std::int32_t featureTableIndex,
                                       std::int64_t featureId) = 0;
    virtual void clearFeatureHighlights() = 0;
    virtual bool setFeatureHighlightStyle(const FeatureHighlightStyle& style) = 0;
    virtual std::uint32_t highlightedFeatureTriangleCount() const noexcept = 0;

    virtual bool visibleBounds(RenderBounds* bounds) const = 0;

    virtual void setRenderBudgetMaxVertices(std::uint64_t maximum) = 0;
    virtual std::uint64_t renderBudgetMaxVertices() const noexcept = 0;
    virtual std::uint64_t renderBudgetCurrentVertices() const noexcept = 0;
    virtual std::uint64_t registeredVertexCount() const noexcept = 0;
    virtual std::uint64_t registeredIndexCount() const noexcept = 0;
    virtual std::uint64_t registeredTextureCount() const noexcept = 0;
    virtual std::uint32_t registeredSceneNodeCount() const noexcept = 0;
    virtual std::uint32_t registeredRenderObjectCount() const noexcept = 0;
};

} // namespace View3D

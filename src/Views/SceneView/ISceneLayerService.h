#pragma once

#include "ILayer.h"

#include <array>
#include <memory>

namespace View3D {

struct ModelMeshDto;

// 场景内容登记与渲染同步：数据插件通过本服务把网格交给共享场景。
class ISceneLayerService {
  public:
    virtual ~ISceneLayerService() = default;

    virtual LayerId addGroup(const QString& name, const LayerId& parentGroup = {}) = 0;
    // sourceCrs 是数据自带的源参照系标识（EPSG 代码或 WKT），仅作展示与回溯：
    // 坐标本身已由 plugin.view3d.crs 在导入时归一到主坐标系。
    virtual LayerId addModelLayer(const QString& name, const QString& renderKey,
                                  const std::shared_ptr<const ModelMeshDto>& mesh,
                                  const LayerId& parentGroup = {},
                                  const QString& sourceCrs = {}) = 0;
    // 一个数据集 = 一个图层：内容按瓦片组流式加载，目录树只看到这一个图层。
    // tileGroupKey 由数据插件决定，渲染端据此路由组级可见性/透明度/偏移。
    // sourceCrs 是数据自带的源参照系（3D Tiles 固定 EPSG:4979、倾斜摄影取
    // metadata 的 SRS），没有参照系的成果传空，属性面板相应留空。
    virtual LayerId addTileLayer(const QString& name, const QString& tileGroupKey,
                                 const LayerId& parentGroup = {},
                                 const QString& sourceCrs = {}) = 0;
    // 地理图层只登记元数据到图层树；网格/纹理由对应的数据服务提交到渲染场景。
    virtual LayerId addImageryLayer(const QString& name, const QString& renderKey,
                                    const LayerId& parentGroup = {}) = 0;
    virtual LayerId addFeatureLayer(const QString& name, const QString& renderKey,
                                    const LayerId& parentGroup = {}) = 0;
    virtual bool removeLayer(const LayerId& layerId) = 0;
    virtual void clear() = 0;

    // 运行期替换模型图层的整块几何（动态投影重放用）：图层 id、分组归属、可见性、
    // 样式与参照系文本全部保留，只换顶点/索引并刷新统计。渲染端不支持时返回 false。
    virtual bool replaceModelLayerGeometry(const LayerId& layerId,
                                           const std::shared_ptr<const ModelMeshDto>& mesh) = 0;

    virtual bool setLayerVisible(const LayerId& layerId, bool visible) = 0;
    virtual bool setLayerOpacity(const LayerId& layerId, float opacity) = 0;
    virtual bool setLayerOffset(const LayerId& layerId,
                                const std::array<double, 3>& offset) = 0;
    virtual bool setLayerVisibleDistance(const LayerId& layerId, double minimumDistance,
                                         double maximumDistance) = 0;
    virtual bool setLayerStyle(const LayerId& layerId, const std::array<float, 3>& tint,
                               bool edgeLinesVisible, float brightness, float contrast) = 0;

    // 拾取只给出渲染键，UI 需要反查它属于哪个图层：模型/地理图层按精确键匹配，
    // 瓦片键 tile:<组>/<瓦片> 归到它所属的组图层。找不到返回空 id。
    virtual LayerId layerByRenderKey(const QString& renderKey) const = 0;
    virtual QString layerName(const LayerId& layerId) const = 0;
    // 图层登记的源参照系（导入时写入）；空表示数据没写参照系或图层不存在。
    virtual QString layerSourceCrs(const LayerId& layerId) const = 0;
    // 参照系框架（源→局部矩阵/原点/说明）：数据侧的框架往往在图层登记后才算出来
    // （3D Tiles 要等首个瓦片确定原点），算好后回填并通知面板刷新。
    virtual bool setLayerReferenceFrame(const LayerId& layerId,
                                        const ILayer::ReferenceFrame& frame) = 0;
    virtual ILayer::ReferenceFrame layerReferenceFrame(const LayerId& layerId) const = 0;
};

} // namespace View3D

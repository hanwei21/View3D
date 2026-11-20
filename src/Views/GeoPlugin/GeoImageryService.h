#pragma once

#include <IImageryService.h>

#include <QString>
#include <map>
#include <memory>

namespace View3D {
class IGeoReference;
class IRenderScene;
struct ModelMeshDto;
} // namespace View3D

namespace View3DGeo {

// 影像底图服务：把 WMTS/TMS/XYZ 瓦片下载并拼成纹理，贴到覆盖给定经纬度范围的网格上。
// MVP 假设 Web Mercator(EPSG:3857) 瓦片；EPSG:4326 global-geodetic 留作扩展点。
// 世界原点与经纬度换算取自 plugin.view3d.crs 发布的 IGeoReference（该插件缺失时为空）。
class GeoImageryService final : public View3D::IImageryService {
  public:
    GeoImageryService(View3D::IRenderScene& scene, View3D::IGeoReference* reference);

    bool addImageryLayer(const View3D::ImagerySource& source,
                         const QString& layerKey) override;
    bool removeImageryLayer(const QString& layerKey) override;
    bool setImageryVisible(const QString& layerKey, bool visible) override;
    bool setImageryOpacity(const QString& layerKey, float opacity) override;
    bool setImageryOrder(const QString& layerKey, int order) override;
    QString lastError() const override { return m_lastError; }

  private:
    // 图层树删除影像图层时只通知渲染场景；以场景里实际存在的 key 为准清掉登记，
    // 否则 setImageryVisible 会对已经不存在的图层返回成功。
    void pruneStaleLayers() const;

    View3D::IRenderScene& m_scene;
    View3D::IGeoReference* m_reference = nullptr;
    QString m_lastError;
    mutable std::map<QString, int> m_order;
};

} // namespace View3DGeo

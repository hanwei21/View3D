#pragma once

#include <QString>

namespace View3D {

// 影像底图 / 在线地图服务 (WMTS / TMS / XYZ)。叠在地形表面(draping) 或
// 作为独立平面/球层。由渲染插件实现(Cesium 的 RasterOverlay / Osg 的地形贴图)。
struct ImagerySource {
    QString name;
    QString urlTemplate;     // XYZ: ".../{z}/{x}/{y}.jpg"  WMTS: KVP 模板
    QString format = QStringLiteral("image/jpeg");
    int minimumLevel = 0;
    int maximumLevel = 18;
    QString crs = QStringLiteral("EPSG:3857"); // 或 EPSG:4326
    QString tileMatrixSet;   // WMTS 专用
    bool tmsOriginBottomLeft = false;          // TMS 瓦片原点在左下为 true
    QString subdomains = QStringLiteral("abc"); // 形如 a/b/c 轮换
    QString referer;          // 部分服务需要 Referer/Token
    QString apiKey;
};

class IImageryService {
  public:
    virtual ~IImageryService() = default;

    virtual bool addImageryLayer(const ImagerySource& source, const QString& layerKey) = 0;
    virtual bool removeImageryLayer(const QString& layerKey) = 0;
    virtual bool setImageryVisible(const QString& layerKey, bool visible) = 0;
    virtual bool setImageryOpacity(const QString& layerKey, float opacity) = 0;
    // 在瓦片流中插入顺序：数字小者绘于底层(贴地)，大者绘于上层。
    virtual bool setImageryOrder(const QString& layerKey, int order) = 0;

    virtual QString lastError() const = 0;
};

} // namespace View3D

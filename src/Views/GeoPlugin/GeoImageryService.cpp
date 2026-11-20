#include "GeoImageryService.h"

#include <IGeoReference.h>

#include <IRenderScene.h>
#include <ModelMesh.h>

#include <QByteArray>
#include <QEventLoop>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace View3DGeo {

namespace {

// MSVC 的 <cmath> 默认不暴露 M_PI（需要 _USE_MATH_DEFINES），这里自带常量。
constexpr double kPi = 3.14159265358979323846;

QByteArray syncGet(QNetworkAccessManager& nam, const QUrl& url)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QByteArray data = reply->readAll();
    reply->deleteLater();
    return data;
}

// 经纬度 -> Web Mercator 瓦片行列（EPSG:3857，256px）。
void lonLatToTile(double lonDeg, double latDeg, int z, int* tx, int* ty)
{
    const double n = std::pow(2.0, z);
    *tx = static_cast<int>(std::floor((lonDeg + 180.0) / 360.0 * n));
    const double latRad = latDeg * kPi / 180.0;
    *ty = static_cast<int>(std::floor(
        (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / kPi) / 2.0 * n));
}

QString expandTemplate(const QString& tpl, int z, int x, int y)
{
    QString url = tpl;
    url.replace(QStringLiteral("{z}"), QString::number(z));
    url.replace(QStringLiteral("{x}"), QString::number(x));
    url.replace(QStringLiteral("{y}"), QString::number(y));
    return url;
}

// 把瓦片块拼成一张 RGBA8888 大图，并写出纹理字节。
bool stitchTiles(const QList<QImage>& tiles, int tilesX, int tilesY, int tileSize,
                QImage* outImage, std::vector<std::byte>* outBytes)
{
    QImage big(tilesX * tileSize, tilesY * tileSize, QImage::Format_RGBA8888);
    big.fill(Qt::transparent);
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            const QImage& tile = tiles[ty * tilesX + tx];
            if (tile.isNull())
                continue;
            const QImage conv = tile.convertToFormat(QImage::Format_RGBA8888);
            const int w = std::min(tileSize, conv.width());
            const int h = std::min(tileSize, conv.height());
            for (int yy = 0; yy < h; ++yy) {
                const uchar* src = conv.scanLine(yy);
                uchar* dst = big.scanLine(ty * tileSize + yy) + tx * tileSize * 4;
                std::memcpy(dst, src, static_cast<size_t>(w) * 4);
            }
        }
    }
    *outImage = big;
    const int total = big.width() * big.height() * 4;
    outBytes->resize(static_cast<size_t>(total));
    for (int yy = 0; yy < big.height(); ++yy) {
        const uchar* src = big.scanLine(yy);
        std::memcpy(outBytes->data() + static_cast<size_t>(yy) * big.width() * 4, src,
                    static_cast<size_t>(big.width()) * 4);
    }
    return true;
}

std::shared_ptr<View3D::ModelMeshDto> buildTexturedMesh(View3D::IGeoReference& ref,
                                                       const std::vector<std::byte>& rgba,
                                                       int texW, int texH,
                                                       double lonMin, double latMin,
                                                       double lonMax, double latMax)
{
    auto mesh = std::make_shared<View3D::ModelMeshDto>();
    const double corners[4][2] = {
        {lonMin, latMin}, {lonMax, latMin}, {lonMax, latMax}, {lonMin, latMax}};
    // 拼出来的大图第一行是北边的瓦片，Ogre 纹理坐标原点在左上，所以 v=0 落在北侧。
    const double uvs[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (int i = 0; i < 4; ++i) {
        double h = 0.0;
        std::array<double, 3> local {};
        ref.lonLatHeightToLocal(corners[i][0], corners[i][1], h, &local);
        View3D::MeshVertexDto v;
        v.x = local[0];
        v.y = local[1];
        v.z = local[2];
        v.hasNormal = true;
        v.ny = 1.0;
        v.hasTexCoord = true;
        v.u = uvs[i][0];
        v.v = uvs[i][1];
        mesh->vertices.push_back(v);
        // 四角留一份经纬度：显示参照系切换时这块地皮跟着重放，而不是停在旧投影上。
        mesh->geoVertices.push_back(corners[i][0]);
        mesh->geoVertices.push_back(corners[i][1]);
        mesh->geoVertices.push_back(h);
    }
    mesh->geoCrs = ref.mainCrs();
    mesh->geoDisplayCrs = ref.displayCrs();
    mesh->indices = {0, 1, 2, 0, 2, 3};
    View3D::ModelTextureDto tex;
    tex.width = texW;
    tex.height = texH;
    tex.rgba8 = rgba;
    mesh->textures.push_back(tex);
    View3D::ModelPrimitiveDto prim;
    prim.firstVertex = 0;
    prim.vertexCount = 4;
    prim.firstIndex = 0;
    prim.indexCount = 6;
    prim.materialIndex = -1;
    prim.baseColorTextureIndex = 0;
    prim.baseColorTexCoord = 0;
    prim.doubleSided = true;
    mesh->primitives.push_back(prim);
    View3D::ModelNodeDto node;
    node.name = QStringLiteral("imagery");
    node.meshIndex = 0;
    node.parentIndex = -1;
    node.firstPrimitive = 0;
    node.primitiveCount = 1;
    mesh->nodes.push_back(node);
    mesh->rootNodes.push_back(0);
    return mesh;
}

} // namespace

GeoImageryService::GeoImageryService(View3D::IRenderScene& scene,
                                     View3D::IGeoReference* reference)
    : m_scene(scene), m_reference(reference)
{
}

void GeoImageryService::pruneStaleLayers() const
{
    if (m_order.empty())
        return;
    const QList<QString> liveKeys = m_scene.modelLayerKeys();
    for (auto it = m_order.begin(); it != m_order.end();) {
        if (liveKeys.contains(it->first))
            ++it;
        else
            it = m_order.erase(it);
    }
}

bool GeoImageryService::addImageryLayer(const View3D::ImagerySource& source,
                                        const QString& layerKey)
{
    m_lastError.clear();
    if (layerKey.isEmpty() || source.urlTemplate.isEmpty()) {
        m_lastError = QStringLiteral("影像图层参数无效");
        return false;
    }
    pruneStaleLayers();
    if (!m_reference) {
        m_lastError = QStringLiteral("缺少参照系服务：请加载 plugin.view3d.crs");
        return false;
    }

    double originLon = 0.0, originLat = 0.0, originH = 0.0;
    if (!m_reference->origin(&originLon, &originLat, &originH)) {
        m_lastError = QStringLiteral("世界原点尚未钉住：请先用「添加在线底图」输入中心点");
        return false;
    }
    const double lonMin = originLon - 0.005;
    const double latMin = originLat - 0.005;
    const double lonMax = originLon + 0.005;
    const double latMax = originLat + 0.005;

    // 选层级：让覆盖范围的瓦片数不超过 8x8（MVP 限制下载量）。
    // 先按最高层级算一遍再逐级降：maximumLevel 不高于 minimumLevel 时也要有有效值。
    int z = qMax(source.minimumLevel, source.maximumLevel);
    int txMin = 0, tyMin = 0, txMax = 0, tyMax = 0;
    lonLatToTile(lonMin, latMin, z, &txMin, &tyMin);
    lonLatToTile(lonMax, latMax, z, &txMax, &tyMax);
    const auto tileFits = [&txMin, &tyMin, &txMax, &tyMax] {
        return (txMax - txMin + 1) <= 8 && (tyMax - tyMin + 1) <= 8;
    };
    while (z > source.minimumLevel && !tileFits()) {
        --z;
        lonLatToTile(lonMin, latMin, z, &txMin, &tyMin);
        lonLatToTile(lonMax, latMax, z, &txMax, &tyMax);
    }

    QNetworkAccessManager nam;
    const int tilesX = txMax - txMin + 1;
    const int tilesY = tyMax - tyMin + 1;
    const int tileSize = 256;
    QList<QImage> tiles;
    tiles.reserve(tilesX * tilesY);
    for (int ty = tyMin; ty <= tyMax; ++ty) {
        for (int tx = txMin; tx <= txMax; ++tx) {
            const int y = source.tmsOriginBottomLeft ? ((1 << z) - 1 - ty) : ty;
            const QUrl url(expandTemplate(source.urlTemplate, z, tx, y));
            const QByteArray bytes = syncGet(nam, url);
            QImage img;
            if (!bytes.isEmpty())
                img.loadFromData(bytes);
            tiles.append(img);
        }
    }

    QImage big;
    std::vector<std::byte> rgba;
    stitchTiles(tiles, tilesX, tilesY, tileSize, &big, &rgba);
    const int texW = big.width();
    const int texH = big.height();
    if (texW <= 0 || texH <= 0) {
        m_lastError = QStringLiteral("影像瓦片拼接为空");
        return false;
    }

    auto mesh = buildTexturedMesh(*m_reference, rgba, texW, texH, lonMin, latMin, lonMax, latMax);
    if (!m_scene.addModelLayer(layerKey, mesh)) {
        m_lastError = QStringLiteral("影像网格提交到渲染场景失败");
        return false;
    }
    m_order[layerKey] = static_cast<int>(m_order.size());
    return true;
}

bool GeoImageryService::removeImageryLayer(const QString& layerKey)
{
    pruneStaleLayers();
    const auto found = m_order.find(layerKey);
    if (found == m_order.end())
        return false;
    m_scene.removeModelLayer(layerKey);
    m_order.erase(found);
    return true;
}

bool GeoImageryService::setImageryVisible(const QString& layerKey, bool visible)
{
    pruneStaleLayers();
    return m_order.count(layerKey) > 0 && m_scene.setModelLayerVisible(layerKey, visible);
}

bool GeoImageryService::setImageryOpacity(const QString& layerKey, float opacity)
{
    pruneStaleLayers();
    return m_order.count(layerKey) > 0 && m_scene.setModelLayerOpacity(layerKey, opacity);
}

bool GeoImageryService::setImageryOrder(const QString& layerKey, int order)
{
    pruneStaleLayers();
    const auto found = m_order.find(layerKey);
    if (found == m_order.end())
        return false;
    found->second = order;
    return true;
}

} // namespace View3DGeo

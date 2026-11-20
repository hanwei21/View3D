#include "GeoReproject.h"

#include <IGeoReference.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace View3DModel {

QString crsLabelOf(const QString& crs)
{
    if (crs.isEmpty())
        return {};
    const QString trimmed = crs.trimmed();
    const QString upper = trimmed.toUpper();
    if (upper.startsWith(QStringLiteral("EPSG:")) || upper.startsWith(QStringLiteral("ESRI:")))
        return trimmed;
    // WKT 里最后一个 AUTHORITY["EPSG",…] 才是坐标系本身的代码，前面的属于椭球/基准面。
    static const QString kAuthorityToken = QStringLiteral("AUTHORITY[\"EPSG\"");
    const int marker = upper.lastIndexOf(kAuthorityToken);
    if (marker >= 0) {
        const int limit = qMin(crs.size(), marker + kAuthorityToken.size() + 8);
        int pos = marker + kAuthorityToken.size();
        while (pos < limit && !crs[pos].isDigit())
            ++pos;
        int end = pos;
        while (end < crs.size() && crs[end].isDigit())
            ++end;
        if (end > pos)
            return QStringLiteral("EPSG:") + crs.mid(pos, end - pos);
    }
    const int nameStart = crs.indexOf(QLatin1Char('"'));
    const int nameEnd = nameStart < 0 ? -1 : crs.indexOf(QLatin1Char('"'), nameStart + 1);
    if (nameEnd > nameStart)
        return crs.mid(nameStart + 1, nameEnd - nameStart - 1);
    return QStringLiteral("未标注参照系");
}

bool reprojectSourceToGeographic(View3D::IGeoReference* geoReference, const QString& sourceCrs,
                                 std::vector<double>* xyz, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!xyz || xyz->empty() || xyz->size() % 3 != 0)
        return fail(QStringLiteral("待变换的坐标数组不合法"));
    if (!geoReference)
        return fail(QStringLiteral("参照系服务未加载（plugin.view3d.crs）"));
    if (sourceCrs.isEmpty())
        return fail(QStringLiteral("数据没有坐标参考系"));

    std::vector<double> buffer(xyz->size());
    std::copy(xyz->begin(), xyz->end(), buffer.begin());
    if (!geoReference->transformToMain(sourceCrs, buffer.data(), buffer.size() / 3))
        return fail(QStringLiteral("源参照系 %1 无法变换到主参照系 %2")
                        .arg(crsLabelOf(sourceCrs), geoReference->mainCrs()));
    // 变换结果必须是合法经纬度：投影坐标落在有效范围外时 PROJ 也可能“成功”返回异常值。
    for (std::size_t i = 0; i < buffer.size(); i += 3) {
        if (!std::isfinite(buffer[i]) || !std::isfinite(buffer[i + 1]) ||
            !std::isfinite(buffer[i + 2]) || std::abs(buffer[i]) > 180.0 ||
            std::abs(buffer[i + 1]) > 90.0)
            return fail(QStringLiteral("源参照系 %1 的变换结果超出经纬度范围")
                            .arg(crsLabelOf(sourceCrs)));
    }
    xyz->swap(buffer);
    return true;
}

bool ensureWorldOrigin(View3D::IGeoReference* geoReference, double centerLonDeg,
                       double centerLatDeg, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!geoReference)
        return fail(QStringLiteral("参照系服务未加载（plugin.view3d.crs）"));
    double lon = 0.0;
    double lat = 0.0;
    double height = 0.0;
    if (geoReference->origin(&lon, &lat, &height))
        return true;   // 已由先加载的数据源钉住：复用，不搬动已提交的网格
    if (!geoReference->setOrigin(centerLonDeg, centerLatDeg, 0.0))
        return fail(QStringLiteral("世界原点无法钉住：数据中心 %1,%2 不合法")
                        .arg(centerLonDeg, 0, 'f', 6)
                        .arg(centerLatDeg, 0, 'f', 6));
    return true;
}

bool geographicToLocalEnu(View3D::IGeoReference* geoReference,
                          const std::vector<double>& geographic, std::vector<double>* outEnu,
                          QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!geoReference)
        return fail(QStringLiteral("参照系服务未加载（plugin.view3d.crs）"));
    if (!outEnu || geographic.empty() || geographic.size() % 3 != 0)
        return fail(QStringLiteral("待转换的经纬度数组不合法"));
    std::vector<double> result;
    result.reserve(geographic.size());
    std::array<double, 3> local {};
    for (std::size_t i = 0; i < geographic.size(); i += 3) {
        if (!geoReference->lonLatHeightToLocal(geographic[i], geographic[i + 1],
                                               geographic[i + 2], &local))
            return fail(QStringLiteral("经纬度转场景局部坐标失败：%1,%2")
                            .arg(geographic[i], 0, 'f', 6)
                            .arg(geographic[i + 1], 0, 'f', 6));
        result.push_back(local[0]);   // 东
        result.push_back(-local[2]);  // 北（场景 +Z 朝南）
        result.push_back(local[1]);   // 上
    }
    outEnu->swap(result);
    return true;
}

namespace {

// 2×2 求逆（行主序 [a00,a01,a10,a11]）。行列式过小判定失败，调用方保留原法线。
bool invert2x2(const std::array<double, 4>& m, std::array<double, 4>* out)
{
    const double det = m[0] * m[3] - m[1] * m[2];
    if (!std::isfinite(det) || std::abs(det) < 1e-18)
        return false;
    *out = {m[3] / det, -m[1] / det, -m[2] / det, m[0] / det};
    return true;
}

std::array<double, 4> multiply2x2(const std::array<double, 4>& a, const std::array<double, 4>& b)
{
    return {a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3],
            a[2] * b[0] + a[3] * b[2], a[2] * b[1] + a[3] * b[3]};
}

// GDAL/影像的网格都带一个恒等根的节点树，只有真的带变换时顶点才不在世界位置上。
bool nodesAreIdentity(const View3D::ModelMeshDto& mesh)
{
    static const std::array<double, 16> identity {1.0, 0.0, 0.0, 0.0,
                                                  0.0, 1.0, 0.0, 0.0,
                                                  0.0, 0.0, 1.0, 0.0,
                                                  0.0, 0.0, 0.0, 1.0};
    for (const View3D::ModelNodeDto& node : mesh.nodes) {
        if (node.worldTransform != identity)
            return false;
    }
    return true;
}

} // namespace

bool cacheGeographicVertices(const View3D::IGeoReference* geoReference,
                             View3D::ModelMeshDto* mesh, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!mesh || !geoReference)
        return fail(QStringLiteral("缺少网格或参照系服务，无法缓存源地理坐标"));
    if (!mesh->geoCrs.isEmpty() &&
        mesh->geoVertices.size() == mesh->vertices.size() * 3)
        return true;   // 已经缓存过（重放不会改动源坐标，无需重建）
    if (!geoReference->ready())
        return fail(QStringLiteral("世界原点未确定，无法反算源地理坐标"));
    // 顶点带真实节点变换时不在世界位置上，逐点反算会得到错误经纬度。
    if (!nodesAreIdentity(*mesh))
        return fail(QStringLiteral("带节点变换的网格不缓存源地理坐标"));
    if (mesh->vertices.size() > modelGeoCacheMaxVertices)
        return fail(QStringLiteral("顶点数 %1 超出源坐标缓存上限 %2，切换投影时将重新读取源文件")
                        .arg(mesh->vertices.size())
                        .arg(modelGeoCacheMaxVertices));
    std::vector<double> cache;
    cache.reserve(mesh->vertices.size() * 3);
    for (std::size_t i = 0; i < mesh->vertices.size(); ++i) {
        const View3D::MeshVertexDto& vertex = mesh->vertices[i];
        const std::array<double, 3> local {vertex.x, vertex.y, vertex.z};
        double lonDeg = 0.0;
        double latDeg = 0.0;
        double heightMeters = 0.0;
        if (!geoReference->localToLonLatHeight(local, &lonDeg, &latDeg, &heightMeters)) {
            return fail(QStringLiteral("局部坐标反算经纬度失败（顶点 %1）").arg(i));
        }
        cache.push_back(lonDeg);
        cache.push_back(latDeg);
        cache.push_back(heightMeters);
    }
    mesh->geoVertices = std::move(cache);
    mesh->geoCrs = geoReference->mainCrs();
    mesh->geoDisplayCrs = geoReference->displayCrs();
    return true;
}

bool replayGeographicVertices(const View3D::IGeoReference* geoReference,
                              View3D::ModelMeshDto* mesh, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!mesh || !geoReference)
        return fail(QStringLiteral("缺少网格或参照系服务，无法重放投影"));
    if (mesh->geoVertices.empty() || mesh->geoVertices.size() != mesh->vertices.size() * 3)
        return fail(QStringLiteral("网格没有源地理坐标缓存"));
    std::vector<double> local;
    if (!geoReference->sourcePointsToLocal(mesh->geoCrs, mesh->geoVertices.data(),
                                          mesh->vertices.size(), &local) ||
        local.size() != mesh->geoVertices.size()) {
        return fail(QStringLiteral("按显示参照系 %1 重放顶点失败")
                        .arg(geoReference->displayCrs()));
    }

    // 法线跟随投影旋转：位置逐点走 PROJ 精确重算，法线只需要旧/新显示参照系在
    // 原点处的平面微分之比（共形投影下就是收敛角旋转 + 比例因子）。
    std::array<double, 4> delta { {1.0, 0.0, 0.0, 1.0} };
    const QString previous = mesh->geoDisplayCrs;
    const QString current = geoReference->displayCrs();
    double originLon = 0.0;
    double originLat = 0.0;
    double originHeight = 0.0;
    if (previous != current && geoReference->origin(&originLon, &originLat, &originHeight)) {
        View3D::IGeoReference::PlaneFrame oldFrame;
        View3D::IGeoReference::PlaneFrame newFrame;
        std::array<double, 4> oldToTrue { {1.0, 0.0, 0.0, 1.0} };
        if (geoReference->planeFrameAt(previous, originLon, originLat, originHeight, &oldFrame) &&
            geoReference->planeFrameAt(current, originLon, originLat, originHeight, &newFrame) &&
            invert2x2(oldFrame.trueToGrid, &oldToTrue)) {
            delta = multiply2x2(newFrame.trueToGrid, oldToTrue);
        }
    }
    const bool rotateNormals = delta[0] != 1.0 || delta[1] != 0.0 || delta[2] != 0.0 ||
        delta[3] != 1.0;

    for (std::size_t i = 0; i < mesh->vertices.size(); ++i) {
        View3D::MeshVertexDto& vertex = mesh->vertices[i];
        vertex.x = local[i * 3];
        vertex.y = local[i * 3 + 1];
        vertex.z = local[i * 3 + 2];
        if (!rotateNormals || !vertex.hasNormal)
            continue;
        // 局部 (x,z) 的水平块为 H=[[d00,-d01],[-d10,d11]]；法线走 H^{-T}，
        // 展开后就是 n.x' = d11·n.x + d10·n.z、n.z' = d01·n.x + d00·n.z，比例归一化掉。
        const double nx = vertex.nx;
        const double nz = vertex.nz;
        vertex.nx = delta[3] * nx + delta[2] * nz;
        vertex.nz = delta[1] * nx + delta[0] * nz;
        const double length = std::sqrt(vertex.nx * vertex.nx + vertex.ny * vertex.ny +
                                        vertex.nz * vertex.nz);
        if (length > 1e-12) {
            vertex.nx /= length;
            vertex.ny /= length;
            vertex.nz /= length;
        }
    }
    mesh->geoDisplayCrs = current;
    const std::uint32_t leafSize = mesh->rayIndexLeafSize > 0 ? mesh->rayIndexLeafSize : 8;
    if (!mesh->rebuildRayIndex(leafSize))
        return fail(QStringLiteral("重放后射线索引重建失败"));
    return true;
}

} // namespace View3DModel

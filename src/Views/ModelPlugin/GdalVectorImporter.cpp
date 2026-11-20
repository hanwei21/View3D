#include "GdalVectorImporter.h"

#include "GeoReproject.h"

#include <IGeoReference.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <cpl_error.h>
#include <ogr_api.h>
#include <ogr_core.h>
#include <ogr_srs_api.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace View3DModel {
namespace {

constexpr std::uint64_t kMaxVertices = 2000000;

ModelReadResult failure(const QString& code, const QString& message, const QString& source) {
    ModelReadResult result;
    result.error.code = code;
    result.error.message = message;
    result.error.source = source;
    return result;
}

void ensureGdalRegistered() {
    static std::once_flag once;
    std::call_once(once, [] {
        const QString applicationDirectory = QCoreApplication::applicationDirPath();
        const QString projData = QDir(applicationDirectory).filePath(QStringLiteral("proj-data"));
        if (QFileInfo::exists(projData) && qEnvironmentVariableIsEmpty("PROJ_DATA"))
            qputenv("PROJ_DATA", projData.toUtf8());
        const QString gdalData = QDir(applicationDirectory).filePath(QStringLiteral("gdal-data"));
        if (QFileInfo::exists(gdalData) && qEnvironmentVariableIsEmpty("GDAL_DATA"))
            qputenv("GDAL_DATA", gdalData.toUtf8());
        GDALAllRegister();
    });
}

QString gdalError() {
    const char* message = CPLGetLastErrorMsg();
    return message && *message ? QString::fromUtf8(message) : QString();
}

struct DatasetCloser {
    void operator()(GDALDatasetH dataset) const {
        if (dataset)
            GDALClose(dataset);
    }
};
using DatasetPtr = std::unique_ptr<std::remove_pointer_t<GDALDatasetH>, DatasetCloser>;

struct VectorPoint {
    double x { 0.0 };
    double y { 0.0 };
    double z { 0.0 };
};

// fid 是图层内的 localId（与要素表 values 的下标一致），layer 是 OGR 图层下标：
// 两者一起才能定位到「哪张要素表的第几个要素」。
struct TaggedPoint { VectorPoint p; std::int64_t fid; std::int32_t layer { -1 }; };

struct TaggedLine {
    std::vector<VectorPoint> points;
    bool polygon { false };
    std::int64_t fid { -1 };
    std::int32_t layer { -1 };
};

struct DisplayPoint {
    double x { 0.0 };
    double y { 0.0 };
    double z { 0.0 };
};

// 每层几何的累积器：点用 Points 图元（无要素），点标记/线/面用 Triangles 图元
// （带要素表索引与逐顶点 featureId），所有要素按 fid 连续追加，三角形不会跨要素边界。
struct LayerAccum {
    std::int32_t tableIndex { -1 };
    std::vector<MeshVertexDto> pointVerts;
    std::vector<MeshVertexDto> markerVerts;
    std::vector<std::uint32_t> markerIdx;
    std::vector<std::int64_t> markerFids;
    std::vector<MeshVertexDto> lineVerts;
    std::vector<std::uint32_t> lineIdx;
    std::vector<std::int64_t> lineFids;
    std::vector<MeshVertexDto> polyVerts;
    std::vector<std::uint32_t> polyIdx;
    std::vector<std::int64_t> polyFids;
};

std::array<double, 3> toDisplay(double x, double y, double z) {
    // 地理数据按 Z-up 处理：SHP/KML 的平面坐标在 XY 平面，显示层是 Y-up。
    return {x, z, -y};
}

QString layerCrs(OGRLayerH layer) {
    OGRSpatialReferenceH srs = layer ? OGR_L_GetSpatialRef(layer) : nullptr;
    if (!srs)
        return {};
    char* wkt = nullptr;
    if (OSRExportToWkt(srs, &wkt) != OGRERR_NONE || !wkt)
        return {};
    const QString result = QString::fromUtf8(wkt);
    CPLFree(wkt);
    return result;
}

// 导入时把源参照系归到主参照系：先按各图层自带的 CRS 批量转到 (经度, 纬度, 椭球高)，
// 用点集中心钉住世界原点，再写成该原点 ENU 切平面下的 (东, 北, 上)。随后的
// toDisplay(x,y,z) 得到场景约定的 (E,U,-N)，于是矢量与影像底图、3D Tiles 共用同一原点。
// 服务缺失、图层没有 CRS 或变换失败时保留源坐标并返回 false，*note 说明原因。
bool reprojectToMainFrame(View3D::IGeoReference* geoReference,
                          const std::vector<QString>& crsOfLayer, std::vector<TaggedPoint>* points,
                          std::vector<TaggedLine>* lines, QString* note) {
    if (!geoReference) {
        if (note)
            *note = QStringLiteral("参照系服务未加载，矢量按源坐标显示");
        return false;
    }
    const std::size_t layerCount = crsOfLayer.size();
    // 逐图层分桶：同一文件里各图层可能写着不同的参照系，必须按图层分别变换。
    std::vector<std::vector<std::size_t>> pointSlots(layerCount);
    std::vector<std::vector<std::array<std::size_t, 2>>> lineSlots(layerCount);
    for (std::size_t i = 0; i < points->size(); ++i) {
        const std::int32_t layer = (*points)[i].layer;
        if (layer >= 0 && static_cast<std::size_t>(layer) < layerCount)
            pointSlots[static_cast<std::size_t>(layer)].push_back(i);
    }
    for (std::size_t i = 0; i < lines->size(); ++i) {
        const std::int32_t layer = (*lines)[i].layer;
        if (layer < 0 || static_cast<std::size_t>(layer) >= layerCount)
            continue;
        // 变量名不能叫 slots：Qt 把 slots 定义成宏，会把这条声明整个吃掉。
        std::vector<std::array<std::size_t, 2>>& lineBucket =
            lineSlots[static_cast<std::size_t>(layer)];
        lineBucket.reserve(lineBucket.size() + (*lines)[i].points.size());
        for (std::size_t j = 0; j < (*lines)[i].points.size(); ++j)
            lineBucket.push_back({i, j});
    }

    std::vector<char> converted(layerCount, 0);
    int convertedLayers = 0;
    int failedLayers = 0;
    int missingCrsLayers = 0;
    QString firstFailure;
    double minLon = std::numeric_limits<double>::max();
    double maxLon = std::numeric_limits<double>::lowest();
    double minLat = minLon;
    double maxLat = maxLon;

    for (std::size_t layer = 0; layer < layerCount; ++layer) {
        const std::size_t count = pointSlots[layer].size() + lineSlots[layer].size();
        if (count == 0)
            continue;
        const QString& crs = crsOfLayer[layer];
        if (crs.isEmpty()) {
            ++missingCrsLayers;
            continue;
        }
        std::vector<double> xyz;
        xyz.reserve(count * 3);
        const auto append = [&xyz](const VectorPoint& p) {
            xyz.push_back(p.x);
            xyz.push_back(p.y);
            xyz.push_back(p.z);
        };
        for (const std::size_t index : pointSlots[layer])
            append((*points)[index].p);
        for (const std::array<std::size_t, 2>& slot : lineSlots[layer])
            append((*lines)[slot[0]].points[slot[1]]);
        QString error;
        if (!reprojectSourceToGeographic(geoReference, crs, &xyz, &error)) {
            ++failedLayers;
            if (firstFailure.isEmpty())
                firstFailure = error;
            continue;
        }
        for (std::size_t i = 0; i < count; ++i) {
            minLon = std::min(minLon, xyz[i * 3]);
            maxLon = std::max(maxLon, xyz[i * 3]);
            minLat = std::min(minLat, xyz[i * 3 + 1]);
            maxLat = std::max(maxLat, xyz[i * 3 + 1]);
        }
        std::size_t cursor = 0;
        const auto stash = [&xyz, &cursor](VectorPoint* p) {
            p->x = xyz[cursor * 3];
            p->y = xyz[cursor * 3 + 1];
            p->z = xyz[cursor * 3 + 2];
            ++cursor;
        };
        for (const std::size_t index : pointSlots[layer])
            stash(&(*points)[index].p);
        for (const std::array<std::size_t, 2>& slot : lineSlots[layer])
            stash(&(*lines)[slot[0]].points[slot[1]]);
        converted[layer] = 1;
        ++convertedLayers;
    }

    if (convertedLayers == 0) {
        if (note) {
            if (!firstFailure.isEmpty())
                *note = firstFailure + QStringLiteral("，矢量按源坐标显示");
            else
                *note = QStringLiteral("矢量图层没有坐标参考系，按源坐标显示");
        }
        return false;
    }

    QString error;
    if (!ensureWorldOrigin(geoReference, (minLon + maxLon) * 0.5, (minLat + maxLat) * 0.5,
                           &error)) {
        if (note)
            *note = error + QStringLiteral("，矢量按源坐标显示");
        return false;
    }
    int localFailed = 0;
    for (std::size_t layer = 0; layer < layerCount; ++layer) {
        if (!converted[layer])
            continue;
        std::vector<double> geographic;
        geographic.reserve((pointSlots[layer].size() + lineSlots[layer].size()) * 3);
        const auto append = [&geographic](const VectorPoint& p) {
            geographic.push_back(p.x);
            geographic.push_back(p.y);
            geographic.push_back(p.z);
        };
        for (const std::size_t index : pointSlots[layer])
            append((*points)[index].p);
        for (const std::array<std::size_t, 2>& slot : lineSlots[layer])
            append((*lines)[slot[0]].points[slot[1]]);
        std::vector<double> enu;
        if (!geographicToLocalEnu(geoReference, geographic, &enu, &error)) {
            converted[layer] = 0;
            ++localFailed;
            if (firstFailure.isEmpty())
                firstFailure = error;
            continue;
        }
        std::size_t cursor = 0;
        const auto writeBack = [&enu, &cursor](VectorPoint* p) {
            p->x = enu[cursor * 3];
            p->y = enu[cursor * 3 + 1];
            p->z = enu[cursor * 3 + 2];
            ++cursor;
        };
        for (const std::size_t index : pointSlots[layer])
            writeBack(&(*points)[index].p);
        for (const std::array<std::size_t, 2>& slot : lineSlots[layer])
            writeBack(&(*lines)[slot[0]].points[slot[1]]);
    }

    if (note) {
        QStringList reasons;
        if (failedLayers > 0 || localFailed > 0)
            reasons.push_back(QStringLiteral("%1 个图层无法变换（%2）")
                                  .arg(failedLayers + localFailed)
                                  .arg(firstFailure));
        if (missingCrsLayers > 0)
            reasons.push_back(QStringLiteral("%1 个图层没有坐标参考系").arg(missingCrsLayers));
        if (!reasons.isEmpty())
            *note = QStringLiteral("部分几何仍按源坐标显示：") + reasons.join(QStringLiteral("；"));
    }
    return true;
}

QString ogrFieldTypeString(OGRFieldType type) {
    switch (type) {
    case OFTInteger: return QStringLiteral("Integer");
    case OFTInteger64: return QStringLiteral("Integer64");
    case OFTReal: return QStringLiteral("Real");
    case OFTString: return QStringLiteral("String");
    case OFTDate: return QStringLiteral("Date");
    case OFTTime: return QStringLiteral("Time");
    case OFTDateTime: return QStringLiteral("DateTime");
    case OFTIntegerList: return QStringLiteral("IntegerList");
    case OFTInteger64List: return QStringLiteral("Integer64List");
    case OFTRealList: return QStringLiteral("RealList");
    case OFTStringList: return QStringLiteral("StringList");
    case OFTBinary: return QStringLiteral("Binary");
    default: return QStringLiteral("Unknown");
    }
}

void collectGeometry(OGRGeometryH geometry, std::vector<TaggedPoint>* points,
                     std::vector<TaggedLine>* lines, std::int32_t layerIndex,
                     std::int64_t fid, std::uint64_t* vertexCount, bool* limitReached);

void collectLine(OGRGeometryH geometry, bool polygon, std::vector<TaggedLine>* lines,
                 std::int32_t layerIndex, std::int64_t fid, std::uint64_t* vertexCount,
                 bool* limitReached) {
    const int count = OGR_G_GetPointCount(geometry);
    if (count < 2)
        return;
    if (*limitReached)
        return;
    TaggedLine line;
    line.polygon = polygon;
    line.fid = fid;
    line.layer = layerIndex;
    line.points.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        // 外壳没有坐标参考系服务：矢量按数据自带坐标直接显示，地理坐标即度当米。
        line.points.push_back({OGR_G_GetX(geometry, i), OGR_G_GetY(geometry, i),
                               OGR_G_GetZ(geometry, i)});
        ++(*vertexCount);
        if (*vertexCount >= kMaxVertices) {
            *limitReached = true;
            return;
        }
    }
    lines->push_back(std::move(line));
}

void collectGeometry(OGRGeometryH geometry, std::vector<TaggedPoint>* points,
                     std::vector<TaggedLine>* lines, std::int32_t layerIndex,
                     std::int64_t fid, std::uint64_t* vertexCount, bool* limitReached) {
    if (!geometry || OGR_G_IsEmpty(geometry))
        return;
    const OGRwkbGeometryType flat = wkbFlatten(OGR_G_GetGeometryType(geometry));
    switch (flat) {
    case wkbPoint:
    case wkbPoint25D: {
        points->push_back({{OGR_G_GetX(geometry, 0), OGR_G_GetY(geometry, 0),
                            OGR_G_GetZ(geometry, 0)}, fid, layerIndex});
        ++(*vertexCount);
        if (*vertexCount >= kMaxVertices)
            *limitReached = true;
        return;
    }
    case wkbLineString:
    case wkbLineString25D:
        collectLine(geometry, false, lines, layerIndex, fid, vertexCount, limitReached);
        return;
    case wkbPolygon:
    case wkbPolygon25D: {
        const int rings = OGR_G_GetGeometryCount(geometry);
        for (int ring = 0; ring < rings; ++ring) {
            collectLine(OGR_G_GetGeometryRef(geometry, ring), true, lines, layerIndex, fid,
                        vertexCount, limitReached);
            if (*limitReached)
                return;
        }
        return;
    }
    case wkbMultiPoint:
    case wkbMultiLineString:
    case wkbMultiPolygon:
    case wkbGeometryCollection: {
        const int count = OGR_G_GetGeometryCount(geometry);
        for (int i = 0; i < count; ++i) {
            collectGeometry(OGR_G_GetGeometryRef(geometry, i), points, lines, layerIndex,
                            fid, vertexCount, limitReached);
            if (*limitReached)
                return;
        }
        return;
    }
    default:
        return;
    }
}

// 追加一个四边形（4 角点）到三角形累积器，并写入逐顶点 featureId。
void appendQuad(std::vector<MeshVertexDto>* verts, std::vector<std::uint32_t>* idx,
                std::vector<std::int64_t>* fids,
                const std::array<std::array<double, 3>, 4>& corners, std::int64_t fid,
                std::uint64_t* vertexCount, bool* limitReached) {
    if (!verts || *limitReached)
        return;
    if (verts->size() + 4 > kMaxVertices) {
        *limitReached = true;
        return;
    }
    const std::uint32_t base = static_cast<std::uint32_t>(verts->size());
    for (const auto& corner : corners) {
        MeshVertexDto vertex;
        vertex.x = corner[0];
        vertex.y = corner[1];
        vertex.z = corner[2];
        vertex.ny = 1.0;
        vertex.hasNormal = true;
        verts->push_back(vertex);
    }
    idx->insert(idx->end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    fids->insert(fids->end(), {fid, fid, fid, fid});
    *vertexCount += 4;
}

// 追加一段有宽度的线（两个端点 + 半宽），生成四边形。
void appendSegment(std::vector<MeshVertexDto>* verts, std::vector<std::uint32_t>* idx,
                  std::vector<std::int64_t>* fids, const DisplayPoint& first,
                  const DisplayPoint& second, double halfWidth, std::int64_t fid,
                  std::uint64_t* vertexCount, bool* limitReached) {
    const double dx = second.x - first.x;
    const double dz = second.z - first.z;
    const double length = std::sqrt(dx * dx + dz * dz);
    if (length < 1e-12)
        return;
    const double offsetX = -dz / length * halfWidth;
    const double offsetZ = dx / length * halfWidth;
    const std::array<std::array<double, 3>, 4> corners {{
        {first.x - offsetX, first.y, first.z - offsetZ},
        {first.x + offsetX, first.y, first.z + offsetZ},
        {second.x + offsetX, second.y, second.z + offsetZ},
        {second.x - offsetX, second.y, second.z - offsetZ},
    }};
    appendQuad(verts, idx, fids, corners, fid, vertexCount, limitReached);
}

void appendPrimitive(ModelMeshDto* mesh,
                     const std::vector<MeshVertexDto>& vertices,
                     const std::vector<std::uint32_t>& indices,
                     const std::vector<std::int64_t>& featureIds,
                     std::int32_t featureTableIndex,
                     const std::array<double, 4>& color,
                     ModelPrimitiveTopology topology, bool doubleSided,
                     std::vector<ModelPrimitiveDto>* primitives) {
    if (!mesh || !primitives || vertices.empty())
        return;
    // Points 拓扑直接按顶点绘制，不需要索引；Triangles 必须有索引。
    if (indices.empty() && topology != ModelPrimitiveTopology::Points)
        return;
    const std::uint32_t firstVertex = static_cast<std::uint32_t>(mesh->vertices.size());
    const std::uint32_t firstIndex = static_cast<std::uint32_t>(mesh->indices.size());
    mesh->vertices.insert(mesh->vertices.end(), vertices.begin(), vertices.end());
    for (std::uint32_t index : indices)
        mesh->indices.push_back(firstVertex + index);
    ModelPrimitiveDto primitive;
    primitive.firstVertex = firstVertex;
    primitive.vertexCount = static_cast<std::uint32_t>(vertices.size());
    primitive.firstIndex = firstIndex;
    primitive.indexCount = static_cast<std::uint32_t>(indices.size());
    primitive.topology = topology;
    primitive.baseColorR = color[0];
    primitive.baseColorG = color[1];
    primitive.baseColorB = color[2];
    primitive.baseColorA = color[3];
    primitive.metallicFactor = 0.0;
    primitive.roughnessFactor = 1.0;
    primitive.doubleSided = doubleSided;
    // 仅三角形图元参与要素拾取解析；点图元不携带要素。
    if (topology == ModelPrimitiveTopology::Triangles && featureTableIndex >= 0) {
        primitive.featureTableIndex = featureTableIndex;
        primitive.featureIds = featureIds;
    }
    primitives->push_back(primitive);
}

} // namespace

QStringList GdalVectorImporter::suffixes() {
    return {QStringLiteral("shp"), QStringLiteral("kml")};
}

bool GdalVectorImporter::supportsSuffix(const QString& suffix) {
    return suffix.compare(QLatin1String("shp"), Qt::CaseInsensitive) == 0 ||
           suffix.compare(QLatin1String("kml"), Qt::CaseInsensitive) == 0;
}

bool GdalVectorImporter::isVectorMagic(const QByteArray& prefix) {
    if (prefix.size() >= 4 && static_cast<std::uint8_t>(prefix[0]) == 0 &&
        static_cast<std::uint8_t>(prefix[1]) == 0 &&
        static_cast<std::uint8_t>(prefix[2]) == 0x27 &&
        static_cast<std::uint8_t>(prefix[3]) == 0x0a)
        return true;
    const QByteArray head = prefix.left(4096).trimmed();
    return head.startsWith(QByteArrayLiteral("<kml")) ||
           head.contains(QByteArrayLiteral("<kml")) ||
           head.contains(QByteArrayLiteral("http://www.opengis.net/kml"));
}

ModelReadResult GdalVectorImporter::importFile(const QString& path,
                                                 const ModelImportOptions& options) const
{
    ensureGdalRegistered();
    const QFileInfo fileInfo(path);
    CPLErrorReset();
    DatasetPtr dataset(static_cast<GDALDatasetH>(GDALOpenEx(
        path.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
        nullptr)));
    if (!dataset) {
        const QString message = gdalError();
        return failure(QStringLiteral("GDAL_VECTOR_OPEN_FAILED"),
                       message.isEmpty() ? QStringLiteral("无法打开矢量文件") : message, path);
    }

    const int layerCount = GDALDatasetGetLayerCount(dataset.get());
    if (layerCount <= 0)
        return failure(QStringLiteral("GDAL_VECTOR_EMPTY"),
                       QStringLiteral("矢量文件中没有图层"), path);

    std::vector<TaggedPoint> points;
    std::vector<TaggedLine> lines;
    std::uint64_t vertexCount = 0;
    bool limitReached = false;
    int featureCount = 0;
    int pointFeatures = 0;
    int lineFeatures = 0;
    int polygonFeatures = 0;
    QStringList crsValues;
    // 图层下标 -> 该图层自带的参照系定义（WKT 或 EPSG 串）；重投影按图层分别做。
    std::vector<QString> crsOfLayer(static_cast<std::size_t>(layerCount));
    std::vector<ModelFeatureTableDto> featureTables;
    // 图层下标 -> 要素表(accums)下标；空图层记为 -1（不进入要素表）。
    std::vector<std::int32_t> tableIndexOfLayer(layerCount, -1);

    // 第一遍：收集几何（带要素 id）与属性，并累计坐标范围。
    std::array<double, 6> bounds {std::numeric_limits<double>::max(),
                                  std::numeric_limits<double>::lowest(),
                                  std::numeric_limits<double>::max(),
                                  std::numeric_limits<double>::lowest(),
                                  std::numeric_limits<double>::max(),
                                  std::numeric_limits<double>::lowest()};
    const auto include = [&bounds](const VectorPoint& point) {
        bounds[0] = std::min(bounds[0], point.x);
        bounds[1] = std::max(bounds[1], point.x);
        bounds[2] = std::min(bounds[2], point.y);
        bounds[3] = std::max(bounds[3], point.y);
        bounds[4] = std::min(bounds[4], point.z);
        bounds[5] = std::max(bounds[5], point.z);
    };

    for (int layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
        OGRLayerH layer = GDALDatasetGetLayer(dataset.get(), layerIndex);
        if (!layer)
            continue;
        const QString crs = layerCrs(layer);
        crsOfLayer[static_cast<std::size_t>(layerIndex)] = crs;
        if (!crs.isEmpty() && !crsValues.contains(crs))
            crsValues.push_back(crs);

        OGRFeatureDefnH defn = OGR_L_GetLayerDefn(layer);
        const int fieldCount = defn ? OGR_FD_GetFieldCount(defn) : 0;

        ModelFeatureTableDto table;
        table.name = QString::fromUtf8(OGR_L_GetName(layer));
        for (int f = 0; f < fieldCount; ++f) {
            OGRFieldDefnH fd = OGR_FD_GetFieldDefn(defn, f);
            ModelFeaturePropertyDto property;
            property.id = QString::fromUtf8(OGR_Fld_GetNameRef(fd));
            property.name = property.id;
            property.type = ogrFieldTypeString(OGR_Fld_GetType(fd));
            table.properties.push_back(property);
        }
        // 统一的 FID 列，便于在属性表里定位要素。
        ModelFeaturePropertyDto fidProperty;
        fidProperty.id = QStringLiteral("FID");
        fidProperty.name = QStringLiteral("FID");
        fidProperty.type = QStringLiteral("Integer64");
        table.properties.push_back(fidProperty);

        OGR_L_ResetReading(layer);
        std::int64_t localId = 0;
        while (true) {
            OGRFeatureH feature = OGR_L_GetNextFeature(layer);
            if (!feature)
                break;
            OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
            if (geometry) {
                const OGRwkbGeometryType flat = wkbFlatten(OGR_G_GetGeometryType(geometry));
                if (flat == wkbPoint || flat == wkbMultiPoint)
                    ++pointFeatures;
                else if (flat == wkbLineString || flat == wkbMultiLineString)
                    ++lineFeatures;
                else if (flat == wkbPolygon || flat == wkbMultiPolygon)
                    ++polygonFeatures;
                collectGeometry(geometry, &points, &lines, layerIndex, localId, &vertexCount,
                                &limitReached);
            }
            // 属性值（含 FID）逐要素填入要素表的 property.values，索引即 localId。
            for (int f = 0; f < fieldCount; ++f) {
                QString value;
                if (OGR_F_IsFieldSetAndNotNull(feature, f))
                    value = QString::fromUtf8(OGR_F_GetFieldAsString(feature, f));
                table.properties[static_cast<std::size_t>(f)].values.push_back(value);
            }
            table.properties.back().values.push_back(QString::number(OGR_F_GetFID(feature)));
            ++localId;
            ++featureCount;
            OGR_F_Destroy(feature);
            if (limitReached)
                break;
        }
        table.featureCount = localId;
        if (localId > 0) {
            tableIndexOfLayer[layerIndex] =
                static_cast<std::int32_t>(featureTables.size());
            featureTables.push_back(std::move(table));
        }
        if (limitReached)
            break;
    }

    if (points.empty() && lines.empty())
        return failure(QStringLiteral("GDAL_VECTOR_NO_GEOMETRY"),
                       QStringLiteral("矢量文件中没有可显示的点/线/面要素"), path);

    // 参照系归一必须在包围盒之前：转成米以后范围才有意义（线宽、原点都靠它）。
    QString crsNote;
    const bool reprojected = reprojectToMainFrame(options.geoReference, crsOfLayer, &points,
                                                 &lines, &crsNote);

    // 一次性整体包围盒（所有图层要素），用于确定局部原点与自适应线宽。
    for (const TaggedPoint& tp : points)
        include(tp.p);
    for (const TaggedLine& tl : lines)
        for (const VectorPoint& vp : tl.points)
            include(vp);

    std::array<double, 3> origin = options.localOrigin;
    const bool originWasZero = origin[0] == 0.0 && origin[1] == 0.0 && origin[2] == 0.0;
    // 已归到主参照系的几何不能再生自己的中心原点：那会把整层挪离世界原点，
    // 与影像底图、3D Tiles 对不上。只有按源坐标显示时才做精度平移。
    if (originWasZero && !reprojected) {
        const double centerX = (bounds[0] + bounds[1]) * 0.5;
        const double centerY = (bounds[2] + bounds[3]) * 0.5;
        if (std::abs(centerX) > 10000.0 || std::abs(centerY) > 10000.0)
            origin = {centerX, centerY, 0.0};
    }

    const auto toDisplayPoint = [&origin](const VectorPoint& point) {
        const std::array<double, 3> display =
            toDisplay(point.x - origin[0], point.y - origin[1], point.z - origin[2]);
        return DisplayPoint {display[0], display[1], display[2]};
    };

    // 第二遍：把几何转成显示坐标，按要素表（图层）聚合成带 featureId 的图元。
    std::vector<LayerAccum> accums(static_cast<std::size_t>(featureTables.size()));
    for (std::size_t t = 0; t < accums.size(); ++t)
        accums[t].tableIndex = static_cast<std::int32_t>(t);

    double minX = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double minZ = std::numeric_limits<double>::max();
    double maxZ = std::numeric_limits<double>::lowest();
    const auto includeDisplay = [&minX, &maxX, &minZ, &maxZ](const DisplayPoint& point) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minZ = std::min(minZ, point.z);
        maxZ = std::max(maxZ, point.z);
    };

    // 要素自带「图层下标 + 图层内 localId」，直接据此归属到要素表，无需再读一遍数据。
    const auto tableForLayer = [&tableIndexOfLayer](std::int32_t layerIndex) -> std::int32_t {
        if (layerIndex < 0 ||
            layerIndex >= static_cast<std::int32_t>(tableIndexOfLayer.size()))
            return -1;
        return tableIndexOfLayer[static_cast<std::size_t>(layerIndex)];
    };

    // 先把所有要素换算到显示坐标并求整体范围，再据范围推导自适应线宽/点标记尺寸。
    for (const TaggedPoint& tp : points) {
        const DisplayPoint dp = toDisplayPoint(tp.p);
        includeDisplay(dp);
    }
    for (const TaggedLine& tl : lines) {
        for (const VectorPoint& p : tl.points) {
            const DisplayPoint dp = toDisplayPoint(p);
            includeDisplay(dp);
        }
    }
    // 自适应线宽/点标记尺寸：随数据范围缩放，保证在米制或经纬度坐标下都可见。
    const double extentX = maxX > minX ? maxX - minX : 0.0;
    const double extentZ = maxZ > minZ ? maxZ - minZ : 0.0;
    double unit = std::max(extentX, extentZ) * 0.0015;
    if (!(unit > 0.0))
        unit = 1.0;
    unit = std::max(unit, 0.01);

    for (const TaggedPoint& tp : points) {
        const DisplayPoint dp = toDisplayPoint(tp.p);
        includeDisplay(dp);
        const std::int32_t ti = tableForLayer(tp.layer);
        if (ti < 0)
            continue;
        const std::int64_t localId = tp.fid;
        LayerAccum& accum = accums[static_cast<std::size_t>(ti)];
        MeshVertexDto vertex;
        vertex.x = dp.x;
        vertex.y = dp.y;
        vertex.z = dp.z;
        accum.pointVerts.push_back(vertex);
        // 点标记四边形（带要素，可点击拾取属性），尺寸随数据范围自适应。
        const double markerHalf = unit * 1.5;
        const std::array<std::array<double, 3>, 4> corners {{
            {dp.x - markerHalf, dp.y, dp.z - markerHalf},
            {dp.x + markerHalf, dp.y, dp.z - markerHalf},
            {dp.x + markerHalf, dp.y, dp.z + markerHalf},
            {dp.x - markerHalf, dp.y, dp.z + markerHalf},
        }};
        appendQuad(&accum.markerVerts, &accum.markerIdx, &accum.markerFids, corners,
                   localId, &vertexCount, &limitReached);
    }
    for (const TaggedLine& tl : lines) {
        std::vector<DisplayPoint> display;
        display.reserve(tl.points.size());
        for (const VectorPoint& p : tl.points)
            display.push_back(toDisplayPoint(p));
        for (const DisplayPoint& dp : display)
            includeDisplay(dp);
        const std::int32_t ti = tableForLayer(tl.layer);
        if (ti < 0)
            continue;
        const std::int64_t localId = tl.fid;
        LayerAccum& accum = accums[static_cast<std::size_t>(ti)];
        std::vector<MeshVertexDto>& verts = tl.polygon ? accum.polyVerts : accum.lineVerts;
        std::vector<std::uint32_t>& idx = tl.polygon ? accum.polyIdx : accum.lineIdx;
        std::vector<std::int64_t>& fids = tl.polygon ? accum.polyFids : accum.lineFids;
        const double half = unit * 0.5;
        for (std::size_t i = 1; i < display.size(); ++i)
            appendSegment(&verts, &idx, &fids, display[i - 1], display[i], half, localId,
                          &vertexCount, &limitReached);
    }

    auto mesh = std::make_shared<ModelMeshDto>();
    std::vector<ModelPrimitiveDto> primitives;
    const std::array<double, 4> pointColor = {1.0, 0.82, 0.18, 1.0};
    const std::array<double, 4> lineColor = {0.10, 0.82, 1.0, 1.0};
    const std::array<double, 4> polyColor = {1.0, 0.55, 0.12, 1.0};
    for (std::size_t t = 0; t < accums.size(); ++t) {
        const LayerAccum& accum = accums[t];
        const std::int32_t tableIndex = static_cast<std::int32_t>(t);
        if (!accum.pointVerts.empty())
            appendPrimitive(mesh.get(), accum.pointVerts, {}, {}, -1, pointColor,
                            ModelPrimitiveTopology::Points, false, &primitives);
        if (!accum.markerVerts.empty())
            appendPrimitive(mesh.get(), accum.markerVerts, accum.markerIdx, accum.markerFids,
                            tableIndex, pointColor, ModelPrimitiveTopology::Triangles, true,
                            &primitives);
        if (!accum.lineVerts.empty())
            appendPrimitive(mesh.get(), accum.lineVerts, accum.lineIdx, accum.lineFids,
                            tableIndex, lineColor, ModelPrimitiveTopology::Triangles, true,
                            &primitives);
        if (!accum.polyVerts.empty())
            appendPrimitive(mesh.get(), accum.polyVerts, accum.polyIdx, accum.polyFids,
                            tableIndex, polyColor, ModelPrimitiveTopology::Triangles, true,
                            &primitives);
    }
    if (primitives.empty())
        return failure(QStringLiteral("GDAL_VECTOR_NO_GEOMETRY"),
                       QStringLiteral("矢量要素没有生成任何可显示图元"), path);
    mesh->primitives = std::move(primitives);
    mesh->featureTables = std::move(featureTables);

    ModelNodeDto root;
    root.name = fileInfo.completeBaseName();
    root.sourceNodeIndex = 0;
    root.meshIndex = 0;
    root.parentIndex = -1;
    root.firstPrimitive = 0;
    root.primitiveCount = static_cast<std::uint32_t>(mesh->primitives.size());
    mesh->nodes.push_back(root);
    mesh->rootNodes.push_back(0);
    mesh->rebuildRayIndex(options.rayIndexLeafSize);

    QVariantMap statistics;
    statistics.insert(QStringLiteral("dataKind"), QStringLiteral("vector"));
    statistics.insert(QStringLiteral("layers"), layerCount);
    statistics.insert(QStringLiteral("features"), featureCount);
    statistics.insert(QStringLiteral("pointFeatures"), pointFeatures);
    statistics.insert(QStringLiteral("lineFeatures"), lineFeatures);
    statistics.insert(QStringLiteral("polygonFeatures"), polygonFeatures);
    statistics.insert(QStringLiteral("vertices"), static_cast<qulonglong>(mesh->vertices.size()));
    QStringList crsLabels;
    for (const QString& crs : crsValues) {
        const QString label = crsLabelOf(crs);
        if (!label.isEmpty() && !crsLabels.contains(label))
            crsLabels.push_back(label);
    }
    statistics.insert(QStringLiteral("crs"),
                      crsLabels.isEmpty() ? QStringLiteral("无")
                                          : crsLabels.join(QStringLiteral("; ")));
    statistics.insert(QStringLiteral("reprojectedToMainFrame"), reprojected);
    statistics.insert(QStringLiteral("displayOriginX"), origin[0]);
    statistics.insert(QStringLiteral("displayOriginY"), origin[1]);
    statistics.insert(QStringLiteral("displayOriginZ"), origin[2]);
    statistics.insert(QStringLiteral("topology"), QStringLiteral("mixed"));
    statistics.insert(QStringLiteral("attributeTables"),
                      static_cast<int>(mesh->featureTables.size()));
    if (!mesh->featureTables.empty())
        statistics.insert(QStringLiteral("attributeFields"),
                          QString::number(mesh->featureTables.front().properties.size()));

    // featureTables 判空必须在 mesh 被移动走之前取：移动后 mesh 是空指针。
    const bool withoutAttributeTables = mesh->featureTables.empty();

    ModelReadResult output;
    output.success = true;
    output.statistics = statistics;
    output.sourceCrs = crsLabels.join(QStringLiteral(" + "));
    output.mesh = std::move(mesh);
    if (crsValues.isEmpty())
        output.warnings.push_back(QStringLiteral("矢量文件没有坐标系信息，已按原始坐标显示"));
    else if (!crsNote.isEmpty())
        output.warnings.push_back(crsNote);
    if (originWasZero && (origin[0] != 0.0 || origin[1] != 0.0)) {
        output.warnings.push_back(
            QStringLiteral("为保证渲染精度，已自动以矢量范围中心作为局部原点"));
    }
    if (limitReached)
        output.warnings.push_back(QStringLiteral("矢量顶点超过 200 万，已截断，仅显示前一部分"));
    if (withoutAttributeTables)
        output.warnings.push_back(QStringLiteral("该矢量没有可读取的图层/要素属性"));
    return output;
}

} // namespace View3DModel

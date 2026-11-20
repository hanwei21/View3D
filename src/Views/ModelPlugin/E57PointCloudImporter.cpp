#include "E57PointCloudImporter.h"


#include <E57Exception.h>
#include <E57Format.h>
#include <E57SimpleReader.h>

#include <QFileInfo>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace View3DModel {
namespace {

ModelReadResult failure(const QString& code, const QString& message, const QString& source) {
    ModelReadResult result;
    result.error.code = code;
    result.error.message = message;
    result.error.source = source;
    return result;
}

struct VoxelKey {
    std::int64_t x { 0 };
    std::int64_t y { 0 };
    std::int64_t z { 0 };
    bool operator==(const VoxelKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct VoxelKeyHash {
    std::size_t operator()(const VoxelKey& key) const {
        std::uint64_t mixed = static_cast<std::uint64_t>(key.x) * 0x9E3779B97F4A7C15ull
                            ^ static_cast<std::uint64_t>(key.y) * 0xC2B2AE3D27D4EB4Full
                            ^ static_cast<std::uint64_t>(key.z) * 0x165667B19E3779F9ull;
        mixed ^= mixed >> 33;
        mixed *= 0xff51afd7ed558ccdull;
        mixed ^= mixed >> 33;
        return static_cast<std::size_t>(mixed);
    }
};

struct ScanRecord {
    std::int64_t index { 0 };
    e57::Data3D header;
};

struct PointTransform {
    std::array<double, 3> translation {};
    double w { 1.0 };
    double x { 0.0 };
    double y { 0.0 };
    double z { 0.0 };
};

PointTransform poseFromHeader(const e57::Data3D& header) {
    PointTransform pose;
    pose.translation = {header.pose.translation.x, header.pose.translation.y,
                        header.pose.translation.z};
    const double length = std::sqrt(header.pose.rotation.w * header.pose.rotation.w +
                                    header.pose.rotation.x * header.pose.rotation.x +
                                    header.pose.rotation.y * header.pose.rotation.y +
                                    header.pose.rotation.z * header.pose.rotation.z);
    if (length > 1e-12) {
        pose.w = header.pose.rotation.w / length;
        pose.x = header.pose.rotation.x / length;
        pose.y = header.pose.rotation.y / length;
        pose.z = header.pose.rotation.z / length;
    }
    return pose;
}

std::array<double, 3> rotate(const PointTransform& pose, const std::array<double, 3>& point) {
    const std::array<double, 3> q {pose.x, pose.y, pose.z};
    const std::array<double, 3> uv {q[1] * point[2] - q[2] * point[1],
                                    q[2] * point[0] - q[0] * point[2],
                                    q[0] * point[1] - q[1] * point[0]};
    const std::array<double, 3> uuv {q[1] * uv[2] - q[2] * uv[1],
                                     q[2] * uv[0] - q[0] * uv[2],
                                     q[0] * uv[1] - q[1] * uv[0]};
    return {point[0] + 2.0 * (pose.w * uv[0] + uuv[0]),
            point[1] + 2.0 * (pose.w * uv[1] + uuv[1]),
            point[2] + 2.0 * (pose.w * uv[2] + uuv[2])};
}

bool hasCartesianCoordinates(const e57::Data3D& header) {
    return header.pointFields.cartesianXField && header.pointFields.cartesianYField &&
           header.pointFields.cartesianZField;
}

bool hasSphericalCoordinates(const e57::Data3D& header) {
    return header.pointFields.sphericalRangeField && header.pointFields.sphericalAzimuthField &&
           header.pointFields.sphericalElevationField;
}

bool hasColor(const e57::Data3D& header) {
    return header.pointFields.colorRedField && header.pointFields.colorGreenField &&
           header.pointFields.colorBlueField;
}

std::uint8_t normalizedColor(std::uint16_t value, double minimum, double maximum) {
    if (std::isfinite(minimum) && std::isfinite(maximum) && maximum > minimum) {
        const double normalized = (static_cast<double>(value) - minimum) / (maximum - minimum);
        return static_cast<std::uint8_t>(std::clamp(std::lround(normalized * 255.0), 0L, 255L));
    }
    // 没有 colorLimits 的文件常见两种写法：0..255 直接存，或 16 位全范围。
    return static_cast<std::uint8_t>(value <= 255 ? value : value >> 8);
}

std::uint8_t normalizedIntensity(double value, double minimum, double maximum) {
    double normalized = 0.0;
    if (std::isfinite(minimum) && std::isfinite(maximum) && maximum > minimum) {
        normalized = (value - minimum) / (maximum - minimum);
    } else if (std::isfinite(value)) {
        // E57 没有强制强度量纲；没有 limits 时按 0..1、0..255、0..65535 三种常见写法解释。
        normalized = value <= 1.0 ? value : (value <= 255.0 ? value / 255.0 : value / 65535.0);
    }
    return static_cast<std::uint8_t>(std::clamp(std::lround(normalized * 255.0), 0L, 255L));
}

bool boundsFinite(const e57::CartesianBounds& bounds) {
    return std::isfinite(bounds.xMinimum) && std::isfinite(bounds.xMaximum) &&
           std::isfinite(bounds.yMinimum) && std::isfinite(bounds.yMaximum) &&
           std::isfinite(bounds.zMinimum) && std::isfinite(bounds.zMaximum);
}

QString boundsText(const std::array<double, 6>& bounds, bool valid) {
    if (!valid)
        return QStringLiteral("unknown");
    return QStringLiteral("%1,%2,%3,%4,%5,%6")
        .arg(bounds[0])
        .arg(bounds[1])
        .arg(bounds[2])
        .arg(bounds[3])
        .arg(bounds[4])
        .arg(bounds[5]);
}

} // namespace

QStringList E57PointCloudImporter::suffixes() {
    return {QStringLiteral("e57")};
}

bool E57PointCloudImporter::supportsSuffix(const QString& suffix) {
    return suffix.compare(QLatin1String("e57"), Qt::CaseInsensitive) == 0;
}

bool E57PointCloudImporter::isCloudMagic(const QByteArray& prefix) {
    return prefix.size() >= 8 && prefix.left(8) == QByteArrayLiteral("ASTM-E57");
}

ModelReadResult E57PointCloudImporter::importFile(const QString& path,
                                                    const ModelImportOptions& options) const
{
    if (!std::isfinite(options.unitScale) || options.unitScale <= 0.0)
        return failure(QStringLiteral("MODEL_IMPORT_OPTIONS_INVALID"),
                       QStringLiteral("模型单位比例无效"), path);
    const QFileInfo fileInfo(path);

    try {
        e57::Reader reader(path.toStdString(), e57::ReaderOptions {});
        if (!reader.IsOpen())
            return failure(QStringLiteral("E57_OPEN_FAILED"),
                           QStringLiteral("无法打开 E57 文件"), path);

        const std::int64_t scanCount = reader.GetData3DCount();
        if (scanCount <= 0)
            return failure(QStringLiteral("E57_NO_SCANS"),
                           QStringLiteral("E57 文件中没有 Data3D 扫描"), path);

        std::vector<ScanRecord> scans;
        scans.reserve(static_cast<std::size_t>(scanCount));
        std::uint64_t total = 0;
        bool hasAnyCoordinates = false;
        std::array<double, 6> bounds {std::numeric_limits<double>::max(),
                                      std::numeric_limits<double>::lowest(),
                                      std::numeric_limits<double>::max(),
                                      std::numeric_limits<double>::lowest(),
                                      std::numeric_limits<double>::max(),
                                      std::numeric_limits<double>::lowest()};
        bool boundsValid = false;
        QString sensor;
        for (std::int64_t index = 0; index < scanCount; ++index) {
            e57::Data3D header;
            if (!reader.ReadData3D(index, header))
                continue;
            if (header.pointCount == 0)
                continue;
            const bool cartesian = hasCartesianCoordinates(header);
            const bool spherical = hasSphericalCoordinates(header);
            if (!cartesian && !spherical)
                continue;
            scans.push_back({index, std::move(header)});
            const e57::Data3D& scan = scans.back().header;
            total += static_cast<std::uint64_t>(scan.pointCount);
            hasAnyCoordinates = true;
            if (sensor.isEmpty() && !scan.sensorVendor.empty())
                sensor = QString::fromStdString(scan.sensorVendor);
            if (!scan.sensorModel.empty()) {
                const QString model = QString::fromStdString(scan.sensorModel);
                if (!sensor.contains(model))
                    sensor = sensor.isEmpty() ? model : sensor + QStringLiteral(" ") + model;
            }
            if (boundsFinite(scan.cartesianBounds)) {
                // 点云常是平面扫描（某一轴范围为 0），所以不能用“三轴都非零”当有效性
                // 条件；这里把包围盒 8 个角按 scan 位姿变换后并入全局范围，抽稀体素
                // 才不会漏掉第二个 scan 的平移。
                const PointTransform pose = poseFromHeader(scan);
                const auto poseCorner = [&pose](double x, double y, double z) {
                    const std::array<double, 3> rotated = rotate(pose, {x, y, z});
                    return std::array<double, 3> {rotated[0] + pose.translation[0],
                                                  rotated[1] + pose.translation[1],
                                                  rotated[2] + pose.translation[2]};
                };
                const double xs[2] = {scan.cartesianBounds.xMinimum,
                                      scan.cartesianBounds.xMaximum};
                const double ys[2] = {scan.cartesianBounds.yMinimum,
                                      scan.cartesianBounds.yMaximum};
                const double zs[2] = {scan.cartesianBounds.zMinimum,
                                      scan.cartesianBounds.zMaximum};
                for (double x : xs) {
                    for (double y : ys) {
                        for (double z : zs) {
                            const std::array<double, 3> corner = poseCorner(x, y, z);
                            bounds[0] = std::min(bounds[0], corner[0]);
                            bounds[1] = std::max(bounds[1], corner[0]);
                            bounds[2] = std::min(bounds[2], corner[1]);
                            bounds[3] = std::max(bounds[3], corner[1]);
                            bounds[4] = std::min(bounds[4], corner[2]);
                            bounds[5] = std::max(bounds[5], corner[2]);
                        }
                    }
                }
                boundsValid = true;
            }
        }
        if (!hasAnyCoordinates || scans.empty() || total == 0)
            return failure(QStringLiteral("E57_NO_POINTS"),
                           QStringLiteral("E57 文件中没有带坐标的点"), path);

        const bool decimate =
            options.pointBudget > 0 && total > static_cast<std::uint64_t>(options.pointBudget);
        double voxelSize = 0.0;
        std::unordered_set<VoxelKey, VoxelKeyHash> occupied;
        if (decimate) {
            const double extentX = (bounds[1] - bounds[0]) * options.unitScale;
            const double extentY = (bounds[3] - bounds[2]) * options.unitScale;
            const double extentZ = (bounds[5] - bounds[4]) * options.unitScale;
            const double volume =
                boundsValid ? extentX * extentY * extentZ : 0.0;
            const double largestExtent = std::max({extentX, extentY, extentZ});
            const double target = static_cast<double>(options.pointBudget) * 0.5;
            if (volume > 0.0 && std::isfinite(volume) && target > 0.0)
                voxelSize = std::cbrt(volume / target);
            else if (largestExtent > 0.0 && std::isfinite(largestExtent) && target > 0.0)
                // 平面/线状点云的体积为 0；退化为按最长边缩放，否则体素会小到几乎
                // 不抽稀，点预算就失去意义。
                voxelSize = largestExtent / std::cbrt(target);
            if (!(voxelSize > 0.0))
                voxelSize = 1.0;
            occupied.reserve(static_cast<std::size_t>(options.pointBudget));
        }

        auto meshDto = std::make_shared<ModelMeshDto>();
        meshDto->vertices.reserve(
            decimate ? static_cast<std::size_t>(options.pointBudget)
                     : static_cast<std::size_t>(std::min<std::uint64_t>(total, 4000000)));

        constexpr std::size_t chunkPoints = 262144;
        std::uint64_t kept = 0;
        std::uint64_t readCount = 0;
        std::uint64_t invalidPoints = 0;
        std::uint64_t rgbPoints = 0;
        std::uint64_t normalPoints = 0;
        for (const ScanRecord& scan : scans) {
            const e57::Data3D& header = scan.header;
            const bool cartesian = hasCartesianCoordinates(header);
            const bool spherical = !cartesian && hasSphericalCoordinates(header);
            if (!cartesian && !spherical)
                continue;
            const bool colorFields = hasColor(header);
            const bool intensityField = header.pointFields.intensityField;
            const bool normalFields = header.pointFields.normalXField &&
                                      header.pointFields.normalYField &&
                                      header.pointFields.normalZField;

            e57::Data3D bufferHeader = header;
            bufferHeader.pointCount = chunkPoints;
            e57::Data3DPointsDouble buffers(bufferHeader);
            e57::CompressedVectorReader vectorReader =
                reader.SetUpData3DPointsData(scan.index, chunkPoints, buffers);
            const PointTransform pose = poseFromHeader(header);
            while (true) {
                const unsigned read = vectorReader.read();
                if (read == 0)
                    break;
                for (unsigned i = 0; i < read; ++i) {
                    ++readCount;
                    const bool invalid =
                        (cartesian && buffers.cartesianInvalidState &&
                         buffers.cartesianInvalidState[i] != 0) ||
                        (!cartesian && buffers.sphericalInvalidState &&
                         buffers.sphericalInvalidState[i] != 0);
                    if (invalid) {
                        ++invalidPoints;
                        continue;
                    }
                    double x = 0.0;
                    double y = 0.0;
                    double z = 0.0;
                    if (cartesian) {
                        x = static_cast<double>(buffers.cartesianX[i]);
                        y = static_cast<double>(buffers.cartesianY[i]);
                        z = static_cast<double>(buffers.cartesianZ[i]);
                    } else {
                        const double range = static_cast<double>(buffers.sphericalRange[i]);
                        const double azimuth = static_cast<double>(buffers.sphericalAzimuth[i]);
                        const double elevation = static_cast<double>(buffers.sphericalElevation[i]);
                        const double horizontal = range * std::cos(elevation);
                        x = horizontal * std::cos(azimuth);
                        y = horizontal * std::sin(azimuth);
                        z = range * std::sin(elevation);
                    }
                    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                        ++invalidPoints;
                        continue;
                    }
                    const std::array<double, 3> posed =
                        rotate(pose, {x, y, z});
                    x = posed[0] + pose.translation[0];
                    y = posed[1] + pose.translation[1];
                    z = posed[2] + pose.translation[2];
                    x *= options.unitScale;
                    y *= options.unitScale;
                    z *= options.unitScale;
                    x -= options.localOrigin[0];
                    y -= options.localOrigin[1];
                    z -= options.localOrigin[2];
                    if (decimate) {
                        const VoxelKey key {
                            static_cast<std::int64_t>(std::floor(x / voxelSize)),
                            static_cast<std::int64_t>(std::floor(y / voxelSize)),
                            static_cast<std::int64_t>(std::floor(z / voxelSize))};
                        if (!occupied.insert(key).second)
                            continue;
                    }
                    MeshVertexDto vertex;
                    vertex.hasNormal = false;
                    if (options.sourceUpAxis == ModelSourceUpAxis::ZUp) {
                        vertex.x = x;
                        vertex.y = z;
                        vertex.z = -y;
                    } else {
                        vertex.x = x;
                        vertex.y = y;
                        vertex.z = z;
                    }
                    if (normalFields) {
                        std::array<double, 3> normal = rotate(
                            pose, {static_cast<double>(buffers.normalX[i]),
                                   static_cast<double>(buffers.normalY[i]),
                                   static_cast<double>(buffers.normalZ[i])});
                        const double normalLength = std::sqrt(
                            normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                        if (normalLength > 1e-12) {
                            normal[0] /= normalLength;
                            normal[1] /= normalLength;
                            normal[2] /= normalLength;
                            if (options.sourceUpAxis == ModelSourceUpAxis::ZUp) {
                                vertex.nx = normal[0];
                                vertex.ny = normal[2];
                                vertex.nz = -normal[1];
                            } else {
                                vertex.nx = normal[0];
                                vertex.ny = normal[1];
                                vertex.nz = normal[2];
                            }
                            vertex.hasNormal = true;
                            ++normalPoints;
                        }
                    }
                    const bool colorValid =
                        !buffers.isColorInvalid || buffers.isColorInvalid[i] == 0;
                    if (colorFields && colorValid && buffers.colorRed && buffers.colorGreen &&
                        buffers.colorBlue) {
                        vertex.hasColor = true;
                        vertex.colorR = normalizedColor(buffers.colorRed[i],
                                                        header.colorLimits.colorRedMinimum,
                                                        header.colorLimits.colorRedMaximum);
                        vertex.colorG = normalizedColor(buffers.colorGreen[i],
                                                        header.colorLimits.colorGreenMinimum,
                                                        header.colorLimits.colorGreenMaximum);
                        vertex.colorB = normalizedColor(buffers.colorBlue[i],
                                                        header.colorLimits.colorBlueMinimum,
                                                        header.colorLimits.colorBlueMaximum);
                        vertex.colorA = 255;
                        ++rgbPoints;
                    } else if (intensityField && buffers.intensity) {
                        const std::uint8_t gray = normalizedIntensity(
                            buffers.intensity[i], header.intensityLimits.intensityMinimum,
                            header.intensityLimits.intensityMaximum);
                        vertex.hasColor = true;
                        vertex.colorR = gray;
                        vertex.colorG = gray;
                        vertex.colorB = gray;
                        vertex.colorA = 255;
                    }
                    meshDto->vertices.push_back(vertex);
                    ++kept;
                }
            }
            vectorReader.close();
        }

        if (kept == 0)
            return failure(
                QStringLiteral("E57_EMPTY_CLOUD"),
                QStringLiteral("没有读到有效点（声明 %1 点，实读 %2 点）")
                    .arg(static_cast<qulonglong>(total))
                    .arg(static_cast<qulonglong>(readCount)),
                path);
        // 图元顶点区间是 32 位索引，超出只能截断，宁可不显示也不要画错。
        if (kept > 0xFFFFFFFEULL)
            return failure(QStringLiteral("E57_TOO_MANY_POINTS"),
                           QStringLiteral("点云超过单个图元可容纳的顶点数，请设置点预算"), path);

        ModelPrimitiveDto primitive;
        primitive.firstVertex = 0;
        primitive.vertexCount = static_cast<std::uint32_t>(kept);
        primitive.firstIndex = 0;
        primitive.indexCount = 0;
        primitive.topology = ModelPrimitiveTopology::Points;
        meshDto->primitives.push_back(primitive);

        ModelNodeDto root;
        root.name = fileInfo.completeBaseName();
        root.sourceNodeIndex = 0;
        root.meshIndex = 0;
        root.parentIndex = -1;
        root.firstPrimitive = 0;
        root.primitiveCount = 1;
        meshDto->nodes.push_back(root);
        meshDto->rootNodes.push_back(0);

        QVariantMap statistics;
        statistics.insert(QStringLiteral("points"), static_cast<qulonglong>(total));
        statistics.insert(QStringLiteral("keptPoints"), static_cast<qulonglong>(kept));
        statistics.insert(QStringLiteral("pointBudget"),
                          static_cast<qulonglong>(options.pointBudget));
        statistics.insert(QStringLiteral("scans"), static_cast<qulonglong>(scans.size()));
        statistics.insert(QStringLiteral("readPoints"), static_cast<qulonglong>(readCount));
        statistics.insert(QStringLiteral("invalidPoints"),
                          static_cast<qulonglong>(invalidPoints));
        statistics.insert(QStringLiteral("rgbPoints"), static_cast<qulonglong>(rgbPoints));
        statistics.insert(QStringLiteral("normalPoints"),
                          static_cast<qulonglong>(normalPoints));
        statistics.insert(QStringLiteral("bounds"), boundsText(bounds, boundsValid));
        statistics.insert(QStringLiteral("sensor"), sensor);
        statistics.insert(QStringLiteral("topology"), QStringLiteral("points"));
        statistics.insert(QStringLiteral("format"), QStringLiteral("e57"));
        if (decimate)
            statistics.insert(QStringLiteral("voxelSize"), voxelSize);

        ModelReadResult output;
        output.success = true;
        output.statistics = statistics;
        output.mesh = std::move(meshDto);
        if (decimate) {
            output.warnings.push_back(
                QStringLiteral("点云 %1 点超过预算 %2，已按体素 %3 抽稀到 %4 点")
                    .arg(static_cast<qulonglong>(total))
                    .arg(static_cast<qulonglong>(options.pointBudget))
                    .arg(voxelSize, 0, 'f', 3)
                    .arg(static_cast<qulonglong>(kept)));
        }
        if (invalidPoints > 0) {
            output.warnings.push_back(QStringLiteral("已跳过 %1 个无效点")
                                          .arg(static_cast<qulonglong>(invalidPoints)));
        }
        return output;
    } catch (const e57::E57Exception& error) {
        return failure(QStringLiteral("E57_READ_FAILED"),
                       QString::fromStdString(error.what()), path);
    } catch (const std::exception& error) {
        return failure(QStringLiteral("E57_READ_FAILED"),
                       QString::fromLocal8Bit(error.what()), path);
    }
}

} // namespace View3DModel

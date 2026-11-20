#include "LasPointCloudImporter.h"


#include <laszip_api.h>

#include <QFileInfo>

#include <cmath>
#include <fstream>
#include <unordered_set>

namespace View3DModel {
namespace {

ModelReadResult failure(const QString& code, const QString& message, const QString& source) {
    ModelReadResult result;
    result.error.code = code;
    result.error.message = message;
    result.error.source = source;
    return result;
}

QString handleError(laszip_POINTER handle, const QString& fallback) {
    laszip_CHAR* message = nullptr;
    if (laszip_get_error(handle, &message) == 0 && message && *message)
        return QString::fromLocal8Bit(message);
    return fallback;
}

// 抽稀用的体素键。三个整数直接混合，不做字符串键，否则每点一次的分配会把读取
// 时间拖到不可接受。
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

bool formatCarriesRgb(laszip_U8 pointDataFormat) {
    return pointDataFormat == 2 || pointDataFormat == 3 || pointDataFormat == 6 ||
           pointDataFormat == 7 || pointDataFormat == 8;
}

} // namespace

QStringList LasPointCloudImporter::suffixes() {
    return {QStringLiteral("las"), QStringLiteral("laz")};
}

bool LasPointCloudImporter::supportsSuffix(const QString& suffix) {
    const QString lower = suffix.toLower();
    return lower == QLatin1String("las") || lower == QLatin1String("laz");
}

bool LasPointCloudImporter::isCloudMagic(const QByteArray& prefix) {
    return prefix.size() >= 4 && prefix.left(4) == QByteArrayLiteral("LASF");
}

ModelReadResult LasPointCloudImporter::importFile(const QString& path,
                                                   const ModelImportOptions& options) const
{
    if (!std::isfinite(options.unitScale) || options.unitScale <= 0.0)
        return failure(QStringLiteral("MODEL_IMPORT_OPTIONS_INVALID"),
                       QStringLiteral("模型单位比例无效"), path);
    const QFileInfo fileInfo(path);
    std::ifstream stream(fileInfo.absoluteFilePath().toStdWString(), std::ios::binary);
    if (!stream.is_open())
        return failure(QStringLiteral("MODEL_FILE_OPEN_FAILED"), QStringLiteral("无法打开点云文件"),
                       path);

    laszip_POINTER handle = nullptr;
    if (laszip_create(&handle) != 0 || !handle)
        return failure(QStringLiteral("LAS_HANDLE_FAILED"), QStringLiteral("LASzip 初始化失败"),
                       path);
    laszip_BOOL compressed = 0;
    if (laszip_open_reader_stream(handle, stream, &compressed) != 0) {
        const QString message = handleError(handle, QStringLiteral("LAS/LAZ 头部解析失败"));
        laszip_destroy(handle);
        return failure(QStringLiteral("LAS_PARSE_FAILED"), message, path);
    }

    laszip_header_struct* header = nullptr;
    laszip_point_struct* point = nullptr;
    laszip_get_header_pointer(handle, &header);
    laszip_get_point_pointer(handle, &point);
    if (!header) {
        laszip_close_reader(handle);
        laszip_destroy(handle);
        return failure(QStringLiteral("LAS_HEADER_MISSING"),
                       QStringLiteral("LASzip 打开成功但没有解析出公共头部"), path);
    }
    // laszip_destroy 会释放头部内存，所以之后任何诊断和统计都只能用这里先取出来的值。
    const QString version =
        QStringLiteral("%1.%2").arg(header->version_major).arg(header->version_minor);
    const int pointFormat = header->point_data_format;
    const int recordLength = header->point_data_record_length;
    const QString generatingSoftware =
        QString::fromLocal8Bit(header->generating_software,
                               static_cast<int>(sizeof(header->generating_software))).trimmed();
    const QString scale = QStringLiteral("%1,%2,%3")
                              .arg(header->x_scale_factor)
                              .arg(header->y_scale_factor)
                              .arg(header->z_scale_factor);
    const QString bounds = QStringLiteral("%1,%2,%3,%4,%5,%6")
                               .arg(header->min_x)
                               .arg(header->max_x)
                               .arg(header->min_y)
                               .arg(header->max_y)
                               .arg(header->min_z)
                               .arg(header->max_z);
    const double extentX = (header->max_x - header->min_x) * options.unitScale;
    const double extentY = (header->max_y - header->min_y) * options.unitScale;
    const double extentZ = (header->max_z - header->min_z) * options.unitScale;

    // LASzip 只有在能确定文件大小时才填 point_count；通过 std::istream 打开时它拿不到
    // 大小，返回 0。所以点数先取头部声明，LASzip 给得出就用它的值，实际读到的条数才是真相。
    std::uint64_t total = header->version_minor >= 4 && header->extended_number_of_point_records
                              ? header->extended_number_of_point_records
                              : header->number_of_point_records;
    laszip_I64 apiCount = 0;
    if (laszip_get_point_count(handle, &apiCount) == 0 && apiCount > 0)
        total = static_cast<std::uint64_t>(apiCount);
    if (total == 0) {
        laszip_close_reader(handle);
        laszip_destroy(handle);
        return failure(QStringLiteral("LAS_EMPTY_CLOUD"),
                       QStringLiteral("头部声明点数为 0（版本 %1 格式 %2 记录长 %3）")
                           .arg(version)
                           .arg(pointFormat)
                           .arg(recordLength),
                       path);
    }

    const bool decimate =
        options.pointBudget > 0 && total > static_cast<std::uint64_t>(options.pointBudget);
    double voxelSize = 0.0;
    std::unordered_set<VoxelKey, VoxelKeyHash> occupied;
    if (decimate) {
        // 目标体素数取预算的一半：留出占用率余量，避免实际保留数远超预算。
        const double volume = extentX * extentY * extentZ;
        const double target = static_cast<double>(options.pointBudget) * 0.5;
        if (volume > 0.0 && std::isfinite(volume) && target > 0.0)
            voxelSize = std::cbrt(volume / target);
        if (!(voxelSize > 0.0))
            voxelSize = 1.0;
        occupied.reserve(static_cast<std::size_t>(options.pointBudget));
    }

    auto meshDto = std::make_shared<ModelMeshDto>();
    meshDto->vertices.reserve(decimate ? static_cast<std::size_t>(options.pointBudget)
                                       : static_cast<std::size_t>(total));

    laszip_F64 coordinates[3] = { 0.0, 0.0, 0.0 };
    std::uint64_t kept = 0;
    std::uint64_t readCount = 0;
    std::uint64_t rgbPoints = 0;
    const bool hasRgbField = formatCarriesRgb(pointFormat);
    for (std::uint64_t index = 0; index < total; ++index) {
        // 读返回非 0 说明记录流到头或已损坏，这时坐标还是上一帧的值，继续读会画出
        // 重复点，所以直接停在这里。
        if (laszip_read_point(handle) != 0)
            break;
        if (laszip_get_coordinates(handle, coordinates) != 0)
            break;
        ++readCount;
        double x = coordinates[0];
        double y = coordinates[1];
        double z = coordinates[2];
        // 重投影必须在单位换算之前：CRS 变换假定源数据是米或度。
        x *= options.unitScale;
        y *= options.unitScale;
        z *= options.unitScale;
        x -= options.localOrigin[0];
        y -= options.localOrigin[1];
        z -= options.localOrigin[2];
        if (decimate) {
            const VoxelKey key { static_cast<std::int64_t>(std::floor(x / voxelSize)),
                                 static_cast<std::int64_t>(std::floor(y / voxelSize)),
                                 static_cast<std::int64_t>(std::floor(z / voxelSize)) };
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
        if (point) {
            // LAS 的 RGB 是 16 位，取高 8 位；没有 RGB 字段时用强度做灰度，
            // 否则整片点云会是一片白，看不出地面与植被的差别。
            if (hasRgbField && (point->rgb[0] | point->rgb[1] | point->rgb[2])) {
                vertex.hasColor = true;
                vertex.colorR = static_cast<std::uint8_t>(point->rgb[0] >> 8);
                vertex.colorG = static_cast<std::uint8_t>(point->rgb[1] >> 8);
                vertex.colorB = static_cast<std::uint8_t>(point->rgb[2] >> 8);
                ++rgbPoints;
            } else {
                vertex.hasColor = true;
                const std::uint8_t gray = static_cast<std::uint8_t>(point->intensity >> 8);
                vertex.colorR = gray;
                vertex.colorG = gray;
                vertex.colorB = gray;
            }
            vertex.colorA = 255;
        }
        meshDto->vertices.push_back(vertex);
        ++kept;
    }
    laszip_close_reader(handle);
    laszip_destroy(handle);

    const std::uint64_t failedReads = total > readCount ? total - readCount : 0;
    if (kept == 0)
        return failure(QStringLiteral("LAS_EMPTY_CLOUD"),
                       QStringLiteral("一条点记录都没读到（声明 %1 点，实读 %2 点，格式 %3，记录长 %4）")
                           .arg(static_cast<qulonglong>(total))
                           .arg(static_cast<qulonglong>(readCount))
                           .arg(pointFormat)
                           .arg(recordLength),
                       path);
    // primitive 的顶点区间是 32 位索引，超出就只能截断，宁可不显示也不要画错。
    if (kept > 0xFFFFFFFEULL)
        return failure(QStringLiteral("LAS_TOO_MANY_POINTS"),
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
    statistics.insert(QStringLiteral("failedReads"), static_cast<qulonglong>(failedReads));
    statistics.insert(QStringLiteral("rgbPoints"), static_cast<qulonglong>(rgbPoints));
    statistics.insert(QStringLiteral("compressed"), compressed != 0);
    statistics.insert(QStringLiteral("topology"), QStringLiteral("points"));
    statistics.insert(QStringLiteral("version"), version);
    statistics.insert(QStringLiteral("pointFormat"), pointFormat);
    statistics.insert(QStringLiteral("recordLength"), recordLength);
    statistics.insert(QStringLiteral("generatingSoftware"), generatingSoftware);
    statistics.insert(QStringLiteral("scale"), scale);
    statistics.insert(QStringLiteral("bounds"), bounds);
    if (decimate)
        statistics.insert(QStringLiteral("voxelSize"), voxelSize);

    ModelReadResult output;
    output.success = true;
    output.statistics = statistics;
    output.mesh = std::move(meshDto);
    if (decimate) {
        output.warnings.push_back(QStringLiteral("点云 %1 点超过预算 %2，已按体素 %3 抽稀到 %4 点")
                                      .arg(static_cast<qulonglong>(total))
                                      .arg(static_cast<qulonglong>(options.pointBudget))
                                      .arg(voxelSize, 0, 'f', 3)
                                      .arg(static_cast<qulonglong>(kept)));
    }
    return output;
}

} // namespace View3DModel

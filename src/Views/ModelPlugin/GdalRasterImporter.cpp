#include "GdalRasterImporter.h"

#include "GeoReproject.h"

#include <IGeoReference.h>

#include <gdal.h>
#include <cpl_error.h>

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
#include <type_traits>
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

void ensureGdalRegistered() {
    static std::once_flag once;
    std::call_once(once, [] {
        // GDAL 的 PROJ 是动态依赖；优先复用应用目录下已部署的 proj.db。
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

struct RasterInfo {
    int width { 0 };
    int height { 0 };
    int bands { 0 };
    bool hasGeoTransform { false };
    double geoTransform[6] { 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
    QString projection;
    QString driver;
    GDALDataType dataType { GDT_Byte };
};

int clampDimension(int value, int fallback) {
    return value > 0 ? value : fallback;
}

std::array<double, 3> toDisplay(double x, double y, double z) {
    // 地理数据按 Z-up 处理：DOM/DEM 与 SHP/KML 的平面坐标都在 XY 平面，
    // 视口是 Y-up，因此 Z 落到 Y、北向 Y 落到 -Z。
    return {x, z, -y};
}

bool readBandAsByte(GDALRasterBandH band, int width, int height,
                    std::vector<std::uint8_t>* output, QString* error) {
    if (!band || !output)
        return false;
    const int sourceWidth = GDALGetRasterBandXSize(band);
    const int sourceHeight = GDALGetRasterBandYSize(band);
    if (sourceWidth <= 0 || sourceHeight <= 0)
        return false;
    const GDALDataType type = GDALGetRasterDataType(band);
    output->assign(static_cast<std::size_t>(width) * height, 0);
    if (type == GDT_Byte) {
        if (GDALRasterIO(band, GF_Read, 0, 0, sourceWidth, sourceHeight, output->data(),
                         width, height, GDT_Byte, 0, 0) != CE_None) {
            if (error)
                *error = gdalError();
            return false;
        }
        return true;
    }
    std::vector<float> values(static_cast<std::size_t>(width) * height, 0.0f);
    if (GDALRasterIO(band, GF_Read, 0, 0, sourceWidth, sourceHeight, values.data(),
                     width, height, GDT_Float32, 0, 0) != CE_None) {
        if (error)
            *error = gdalError();
        return false;
    }
    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    for (float value : values) {
        if (!std::isfinite(value))
            continue;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    if (!(maximum > minimum)) {
        maximum = minimum + 1.0f;
    }
    const float span = maximum - minimum;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float value = values[i];
        const float normalized = std::isfinite(value) ? (value - minimum) / span : 0.0f;
        (*output)[i] = static_cast<std::uint8_t>(
            std::clamp(std::lround(normalized * 255.0f), 0L, 255L));
    }
    return true;
}

bool readElevation(GDALRasterBandH band, int width, int height,
                   std::vector<float>* output, QString* error) {
    if (!band || !output)
        return false;
    output->assign(static_cast<std::size_t>(width) * height, 0.0f);
    if (GDALRasterIO(band, GF_Read, 0, 0, GDALGetRasterBandXSize(band),
                     GDALGetRasterBandYSize(band), output->data(), width, height,
                     GDT_Float32, 0, 0) != CE_None) {
        if (error)
            *error = gdalError();
        return false;
    }
    int hasScale = 0;
    int hasOffset = 0;
    const double scale = GDALGetRasterScale(band, &hasScale);
    const double offset = GDALGetRasterOffset(band, &hasOffset);
    int hasNoData = 0;
    const double noData = GDALGetRasterNoDataValue(band, &hasNoData);
    const double effectiveScale = hasScale && std::isfinite(scale) && scale != 0.0 ? scale : 1.0;
    const double effectiveOffset = hasOffset && std::isfinite(offset) ? offset : 0.0;
    for (float& value : *output) {
        if (!std::isfinite(value) || (hasNoData && static_cast<double>(value) == noData)) {
            value = 0.0f;
            continue;
        }
        value = static_cast<float>(static_cast<double>(value) * effectiveScale +
                                   effectiveOffset);
        if (!std::isfinite(value))
            value = 0.0f;
    }
    return true;
}

std::array<int, 3> colorBands(GDALDatasetH dataset) {
    int red = 1;
    int green = 2;
    int blue = 3;
    for (int index = 1; index <= GDALGetRasterCount(dataset); ++index) {
        const GDALColorInterp interpretation =
            GDALGetRasterColorInterpretation(GDALGetRasterBand(dataset, index));
        if (interpretation == GCI_RedBand)
            red = index;
        else if (interpretation == GCI_GreenBand)
            green = index;
        else if (interpretation == GCI_BlueBand)
            blue = index;
    }
    return {red, green, blue};
}

int alphaBand(GDALDatasetH dataset) {
    for (int index = 1; index <= GDALGetRasterCount(dataset); ++index)
        if (GDALGetRasterColorInterpretation(GDALGetRasterBand(dataset, index)) ==
            GCI_AlphaBand)
            return index;
    return 0;
}

bool buildImageTexture(GDALDatasetH dataset, int textureWidth, int textureHeight,
                       ModelTextureDto* texture, QString* error) {
    if (!texture)
        return false;
    texture->width = textureWidth;
    texture->height = textureHeight;
    texture->rgba8.resize(static_cast<std::size_t>(textureWidth) * textureHeight * 4);
    auto* destination = reinterpret_cast<std::uint8_t*>(texture->rgba8.data());

    const int bandCount = GDALGetRasterCount(dataset);
    GDALRasterBandH firstBand = GDALGetRasterBand(dataset, 1);
    GDALColorTableH palette = firstBand ? GDALGetRasterColorTable(firstBand) : nullptr;
    if (palette && bandCount >= 1) {
        std::vector<std::uint8_t> indices;
        if (!readBandAsByte(firstBand, textureWidth, textureHeight, &indices, error))
            return false;
        for (std::size_t i = 0; i < indices.size(); ++i) {
            GDALColorEntry entry {0, 0, 0, 255};
            if (GDALGetColorEntryAsRGB(palette, indices[i], &entry) == 0) {
                entry.c1 = entry.c2 = entry.c3 = 0;
                entry.c4 = 255;
            }
            destination[i * 4 + 0] = static_cast<std::uint8_t>(entry.c1);
            destination[i * 4 + 1] = static_cast<std::uint8_t>(entry.c2);
            destination[i * 4 + 2] = static_cast<std::uint8_t>(entry.c3);
            destination[i * 4 + 3] = static_cast<std::uint8_t>(entry.c4);
        }
        return true;
    }

    std::vector<std::uint8_t> red;
    std::vector<std::uint8_t> green;
    std::vector<std::uint8_t> blue;
    if (bandCount >= 3) {
        const auto bands = colorBands(dataset);
        if (!readBandAsByte(GDALGetRasterBand(dataset, bands[0]), textureWidth, textureHeight,
                            &red, error) ||
            !readBandAsByte(GDALGetRasterBand(dataset, bands[1]), textureWidth, textureHeight,
                            &green, error) ||
            !readBandAsByte(GDALGetRasterBand(dataset, bands[2]), textureWidth, textureHeight,
                            &blue, error))
            return false;
    } else {
        // 单波段/双波段的灰度影像。
        if (!readBandAsByte(firstBand, textureWidth, textureHeight, &red, error))
            return false;
        green = red;
        blue = red;
    }
    std::vector<std::uint8_t> alpha;
    const int alphaIndex = alphaBand(dataset);
    if (alphaIndex > 0 &&
        !readBandAsByte(GDALGetRasterBand(dataset, alphaIndex), textureWidth, textureHeight,
                        &alpha, error))
        return false;
    for (std::size_t i = 0; i < red.size(); ++i) {
        destination[i * 4 + 0] = red[i];
        destination[i * 4 + 1] = green[i];
        destination[i * 4 + 2] = blue[i];
        destination[i * 4 + 3] = alpha.empty() ? 255 : alpha[i];
    }
    return true;
}

void buildTerrainTexture(const std::vector<float>& heights, int width, int height,
                         ModelTextureDto* texture) {
    if (!texture)
        return;
    texture->width = width;
    texture->height = height;
    texture->rgba8.resize(static_cast<std::size_t>(width) * height * 4);
    auto* destination = reinterpret_cast<std::uint8_t*>(texture->rgba8.data());
    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    for (float value : heights) {
        if (!std::isfinite(value))
            continue;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    if (!(maximum > minimum))
        maximum = minimum + 1.0f;
    const float span = maximum - minimum;
    const std::array<double, 3> light {-0.45, -0.55, 0.70};
    const double lightLength = std::sqrt(light[0] * light[0] + light[1] * light[1] +
                                         light[2] * light[2]);
    const std::array<double, 3> normalizedLight {
        light[0] / lightLength, light[1] / lightLength, light[2] / lightLength};
    const auto heightAt = [&heights, width, height](int x, int y) {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return heights[static_cast<std::size_t>(y) * width + x];
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double dzdx = (heightAt(x + 1, y) - heightAt(x - 1, y)) * 0.5;
            const double dzdy = (heightAt(x, y + 1) - heightAt(x, y - 1)) * 0.5;
            const std::array<double, 3> normal {-dzdx, -dzdy, 1.0};
            const double normalLength =
                std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                          normal[2] * normal[2]);
            const double shade =
                0.35 + 0.65 * std::max(0.0, (normal[0] * normalizedLight[0] +
                                             normal[1] * normalizedLight[1] +
                                             normal[2] * normalizedLight[2]) /
                                                normalLength);
            const float normalized = std::clamp((heightAt(x, y) - minimum) / span, 0.0f, 1.0f);
            const std::array<double, 3> low {0.20, 0.32, 0.20};
            const std::array<double, 3> middle {0.52, 0.45, 0.30};
            const std::array<double, 3> high {0.92, 0.92, 0.88};
            std::array<double, 3> color {};
            for (int channel = 0; channel < 3; ++channel) {
                color[channel] = normalized < 0.5f
                                     ? low[channel] + (middle[channel] - low[channel]) *
                                                          normalized * 2.0f
                                     : middle[channel] + (high[channel] - middle[channel]) *
                                                             (normalized - 0.5f) * 2.0f;
            }
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
            destination[offset + 0] = static_cast<std::uint8_t>(
                std::clamp(std::lround(color[0] * shade * 255.0), 0L, 255L));
            destination[offset + 1] = static_cast<std::uint8_t>(
                std::clamp(std::lround(color[1] * shade * 255.0), 0L, 255L));
            destination[offset + 2] = static_cast<std::uint8_t>(
                std::clamp(std::lround(color[2] * shade * 255.0), 0L, 255L));
            destination[offset + 3] = 255;
        }
    }
}

} // namespace

QStringList GdalRasterImporter::suffixes() {
    return {QStringLiteral("tif"), QStringLiteral("tiff")};
}

bool GdalRasterImporter::supportsSuffix(const QString& suffix) {
    return suffix.compare(QLatin1String("tif"), Qt::CaseInsensitive) == 0 ||
           suffix.compare(QLatin1String("tiff"), Qt::CaseInsensitive) == 0;
}

bool GdalRasterImporter::isRasterMagic(const QByteArray& prefix) {
    if (prefix.size() < 4)
        return false;
    return (prefix[0] == 'I' && prefix[1] == 'I' && prefix[2] == 42 && prefix[3] == 0) ||
           (prefix[0] == 'M' && prefix[1] == 'M' && prefix[2] == 0 && prefix[3] == 42) ||
           (prefix[0] == 'I' && prefix[1] == 'I' && prefix[2] == 43 && prefix[3] == 0) ||
           (prefix[0] == 'M' && prefix[1] == 'M' && prefix[2] == 0 && prefix[3] == 43);
}

ModelReadResult GdalRasterImporter::importFile(const QString& path,
                                                 const ModelImportOptions& options) const
{
    ensureGdalRegistered();
    const QFileInfo fileInfo(path);
    CPLErrorReset();
    DatasetPtr dataset(static_cast<GDALDatasetH>(GDALOpenEx(
        path.toUtf8().constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr,
        nullptr)));
    if (!dataset) {
        const QString message = gdalError();
        return failure(QStringLiteral("GDAL_RASTER_OPEN_FAILED"),
                       message.isEmpty() ? QStringLiteral("无法打开栅格文件") : message, path);
    }

    RasterInfo info;
    info.width = GDALGetRasterXSize(dataset.get());
    info.height = GDALGetRasterYSize(dataset.get());
    info.bands = GDALGetRasterCount(dataset.get());
    if (info.width <= 0 || info.height <= 0 || info.bands <= 0)
        return failure(QStringLiteral("GDAL_RASTER_EMPTY"),
                       QStringLiteral("栅格没有可读的波段或尺寸"), path);
    info.hasGeoTransform = GDALGetGeoTransform(dataset.get(), info.geoTransform) == CE_None;
    const char* projection = GDALGetProjectionRef(dataset.get());
    if (projection && *projection)
        info.projection = QString::fromUtf8(projection);
    GDALDriverH driver = GDALGetDatasetDriver(dataset.get());
    if (driver)
        info.driver = QString::fromLatin1(GDALGetDriverShortName(driver));
    info.dataType = GDALGetRasterDataType(GDALGetRasterBand(dataset.get(), 1));

    GDALRasterBandH firstBand = GDALGetRasterBand(dataset.get(), 1);
    const bool paletted = firstBand && GDALGetRasterColorTable(firstBand);
    // 单波段 Byte 更可能是灰度 DOM；Float/Int 高程才按 DEM/DSM 处理。
    const bool terrain = info.bands == 1 && !paletted && info.dataType != GDT_Byte;

    const int maxGrid = 256;
    const double gridScale =
        std::min(1.0, static_cast<double>(maxGrid) / std::max(info.width, info.height));
    const int gridWidth =
        std::clamp(static_cast<int>(std::lround(info.width * gridScale)), 2, maxGrid);
    const int gridHeight =
        std::clamp(static_cast<int>(std::lround(info.height * gridScale)), 2, maxGrid);

    std::vector<float> heights;
    if (terrain &&
        !readElevation(firstBand, gridWidth, gridHeight, &heights, nullptr))
        return failure(QStringLiteral("GDAL_RASTER_READ_FAILED"),
                       QStringLiteral("读取 DEM/DSM 高程失败"), path);

    ModelTextureDto texture;
    QString textureError;
    if (terrain) {
        buildTerrainTexture(heights, gridWidth, gridHeight, &texture);
    } else {
        int textureWidth = info.width;
        int textureHeight = info.height;
        const int maxTexture = 2048;
        if (textureWidth > maxTexture || textureHeight > maxTexture) {
            const double scale = std::min(static_cast<double>(maxTexture) / textureWidth,
                                          static_cast<double>(maxTexture) / textureHeight);
            textureWidth = std::max(2, static_cast<int>(std::lround(textureWidth * scale)));
            textureHeight = std::max(2, static_cast<int>(std::lround(textureHeight * scale)));
        }
        if (!buildImageTexture(dataset.get(), textureWidth, textureHeight, &texture,
                               &textureError)) {
            return failure(QStringLiteral("GDAL_RASTER_READ_FAILED"),
                           textureError.isEmpty() ? QStringLiteral("读取影像波段失败")
                                                  : textureError,
                           path);
        }
    }

    // 栅格节点先按源参照系归到主参照系（米），成功后不再做自身的中心平移：
    // 自己减中心会把整幅影像挪离世界原点，与矢量、影像底图、3D Tiles 对不上。
    // 没有参照系服务或源没写投影时退回旧行为：大坐标平移到中心附近，度按原值当米用。
    const double centerX = info.geoTransform[0] +
                           info.width * 0.5 * info.geoTransform[1] +
                           info.height * 0.5 * info.geoTransform[2];
    const double centerY = info.geoTransform[3] +
                           info.width * 0.5 * info.geoTransform[4] +
                           info.height * 0.5 * info.geoTransform[5];
    std::array<double, 3> origin = options.localOrigin;
    const bool originWasZero =
        origin[0] == 0.0 && origin[1] == 0.0 && origin[2] == 0.0;

    auto mesh = std::make_shared<ModelMeshDto>();
    mesh->vertices.reserve(static_cast<std::size_t>(gridWidth) * gridHeight);
    const std::size_t nodeCount = static_cast<std::size_t>(gridWidth) * gridHeight;
    std::vector<std::array<double, 3>> positions(nodeCount);
    std::vector<double> nodeXyz(nodeCount * 3);
    const auto heightAt = [&heights, gridWidth, gridHeight](int x, int y) {
        x = std::clamp(x, 0, gridWidth - 1);
        y = std::clamp(y, 0, gridHeight - 1);
        return heights[static_cast<std::size_t>(y) * gridWidth + x];
    };
    for (int row = 0; row < gridHeight; ++row) {
        const double sourceRow =
            gridHeight > 1 ? row * (info.height - 1.0) / (gridHeight - 1) : 0.0;
        for (int column = 0; column < gridWidth; ++column) {
            const double sourceColumn =
                gridWidth > 1 ? column * (info.width - 1.0) / (gridWidth - 1) : 0.0;
            const std::size_t index = static_cast<std::size_t>(row) * gridWidth + column;
            nodeXyz[index * 3 + 0] = info.geoTransform[0] +
                sourceColumn * info.geoTransform[1] + sourceRow * info.geoTransform[2];
            nodeXyz[index * 3 + 1] = info.geoTransform[3] +
                sourceColumn * info.geoTransform[4] + sourceRow * info.geoTransform[5];
            nodeXyz[index * 3 + 2] = terrain ? heightAt(column, row) : 0.0;
        }
    }

    QString crsNote;
    std::vector<double> enu;
    // 变换写进副本：中途失败时 nodeXyz 仍是源坐标，退回旧行为才不会把度数当源坐标显示。
    std::vector<double> geographic = nodeXyz;
    // 没有地理变换时节点坐标是像素号，不是经纬度，不能拿去变换。
    bool reprojected =
        info.hasGeoTransform &&
        reprojectSourceToGeographic(options.geoReference, info.projection, &geographic, &crsNote);
    if (reprojected) {
        double minLon = geographic[0];
        double maxLon = geographic[0];
        double minLat = geographic[1];
        double maxLat = geographic[1];
        for (std::size_t i = 0; i < nodeCount; ++i) {
            minLon = std::min(minLon, geographic[i * 3]);
            maxLon = std::max(maxLon, geographic[i * 3]);
            minLat = std::min(minLat, geographic[i * 3 + 1]);
            maxLat = std::max(maxLat, geographic[i * 3 + 1]);
        }
        reprojected = ensureWorldOrigin(options.geoReference, (minLon + maxLon) * 0.5,
                                        (minLat + maxLat) * 0.5, &crsNote) &&
            geographicToLocalEnu(options.geoReference, geographic, &enu, &crsNote);
    }
    if (reprojected) {
        for (std::size_t i = 0; i < nodeCount; ++i)
            positions[i] = toDisplay(enu[i * 3], enu[i * 3 + 1], enu[i * 3 + 2]);
    } else {
        if (originWasZero && (std::abs(centerX) > 10000.0 || std::abs(centerY) > 10000.0))
            origin = {centerX, centerY, 0.0};
        if (crsNote.isEmpty() && info.projection.isEmpty())
            crsNote = QStringLiteral("栅格没有坐标参考系");
        for (std::size_t i = 0; i < nodeCount; ++i)
            positions[i] = toDisplay(nodeXyz[i * 3] - origin[0], nodeXyz[i * 3 + 1] - origin[1],
                                     nodeXyz[i * 3 + 2] - origin[2]);
    }

    for (int row = 0; row < gridHeight; ++row) {
        for (int column = 0; column < gridWidth; ++column) {
            const std::size_t index = static_cast<std::size_t>(row) * gridWidth + column;
            MeshVertexDto vertex;
            vertex.x = positions[index][0];
            vertex.y = positions[index][1];
            vertex.z = positions[index][2];
            vertex.u = gridWidth > 1 ? static_cast<double>(column) / (gridWidth - 1) : 0.0;
            vertex.v = gridHeight > 1 ? 1.0 - static_cast<double>(row) / (gridHeight - 1)
                                      : 0.0;
            vertex.hasTexCoord = true;
            if (terrain) {
                const std::array<double, 3>& left =
                    positions[static_cast<std::size_t>(row) * gridWidth +
                              std::max(0, column - 1)];
                const std::array<double, 3>& right =
                    positions[static_cast<std::size_t>(row) * gridWidth +
                              std::min(gridWidth - 1, column + 1)];
                const std::array<double, 3>& up =
                    positions[static_cast<std::size_t>(std::max(0, row - 1)) * gridWidth +
                              column];
                const std::array<double, 3>& down =
                    positions[static_cast<std::size_t>(std::min(gridHeight - 1, row + 1)) *
                                  gridWidth +
                              column];
                const std::array<double, 3> du {right[0] - left[0], right[1] - left[1],
                                                right[2] - left[2]};
                const std::array<double, 3> dv {down[0] - up[0], down[1] - up[1],
                                                down[2] - up[2]};
                std::array<double, 3> normal {dv[1] * du[2] - dv[2] * du[1],
                                              dv[2] * du[0] - dv[0] * du[2],
                                              dv[0] * du[1] - dv[1] * du[0]};
                const double length = std::sqrt(normal[0] * normal[0] +
                                                normal[1] * normal[1] +
                                                normal[2] * normal[2]);
                if (length > 1e-12) {
                    normal[0] /= length;
                    normal[1] /= length;
                    normal[2] /= length;
                    if (normal[1] < 0.0) {
                        normal[0] = -normal[0];
                        normal[1] = -normal[1];
                        normal[2] = -normal[2];
                    }
                    vertex.nx = normal[0];
                    vertex.ny = normal[1];
                    vertex.nz = normal[2];
                    vertex.hasNormal = true;
                }
            } else {
                vertex.ny = 1.0;
                vertex.hasNormal = true;
            }
            mesh->vertices.push_back(vertex);
        }
    }
    mesh->indices.reserve(static_cast<std::size_t>(gridWidth - 1) * (gridHeight - 1) * 6);
    for (int row = 0; row + 1 < gridHeight; ++row) {
        for (int column = 0; column + 1 < gridWidth; ++column) {
            const std::uint32_t topLeft =
                static_cast<std::uint32_t>(row * gridWidth + column);
            const std::uint32_t topRight = topLeft + 1;
            const std::uint32_t bottomLeft =
                static_cast<std::uint32_t>((row + 1) * gridWidth + column);
            const std::uint32_t bottomRight = bottomLeft + 1;
            mesh->indices.insert(mesh->indices.end(),
                                 {topLeft, bottomLeft, topRight, topRight, bottomLeft,
                                  bottomRight});
        }
    }

    ModelPrimitiveDto primitive;
    primitive.firstVertex = 0;
    primitive.vertexCount = static_cast<std::uint32_t>(mesh->vertices.size());
    primitive.firstIndex = 0;
    primitive.indexCount = static_cast<std::uint32_t>(mesh->indices.size());
    primitive.topology = ModelPrimitiveTopology::Triangles;
    primitive.baseColorTextureIndex = 0;
    primitive.baseColorR = 1.0;
    primitive.baseColorG = 1.0;
    primitive.baseColorB = 1.0;
    primitive.baseColorA = 1.0;
    primitive.metallicFactor = 0.0;
    primitive.roughnessFactor = 1.0;
    primitive.doubleSided = true;
    mesh->primitives.push_back(primitive);
    const int textureWidth = texture.width;
    const int textureHeight = texture.height;
    mesh->textures.push_back(std::move(texture));

    ModelNodeDto root;
    root.name = fileInfo.completeBaseName();
    root.sourceNodeIndex = 0;
    root.meshIndex = 0;
    root.parentIndex = -1;
    root.firstPrimitive = 0;
    root.primitiveCount = 1;
    mesh->nodes.push_back(root);
    mesh->rootNodes.push_back(0);
    mesh->rebuildRayIndex(options.rayIndexLeafSize);

    QVariantMap statistics;
    statistics.insert(QStringLiteral("dataKind"), QStringLiteral("raster"));
    statistics.insert(QStringLiteral("rasterKind"),
                      terrain ? QStringLiteral("dem") : QStringLiteral("dom"));
    statistics.insert(QStringLiteral("width"), info.width);
    statistics.insert(QStringLiteral("height"), info.height);
    statistics.insert(QStringLiteral("bands"), info.bands);
    statistics.insert(QStringLiteral("driver"), info.driver);
    statistics.insert(QStringLiteral("dataType"),
                      QString::fromLatin1(GDALGetDataTypeName(info.dataType)));
    statistics.insert(QStringLiteral("gridWidth"), gridWidth);
    statistics.insert(QStringLiteral("gridHeight"), gridHeight);
    statistics.insert(QStringLiteral("textureWidth"), textureWidth);
    statistics.insert(QStringLiteral("textureHeight"), textureHeight);
    statistics.insert(QStringLiteral("hasGeoTransform"), info.hasGeoTransform);
    statistics.insert(QStringLiteral("crs"),
                      info.projection.isEmpty() ? QStringLiteral("无")
                                                : crsLabelOf(info.projection));
    statistics.insert(QStringLiteral("reprojectedToMainFrame"), reprojected);
    statistics.insert(QStringLiteral("displayOriginX"), origin[0]);
    statistics.insert(QStringLiteral("displayOriginY"), origin[1]);
    statistics.insert(QStringLiteral("displayOriginZ"), origin[2]);
    statistics.insert(QStringLiteral("topology"), QStringLiteral("triangles"));

    ModelReadResult output;
    output.success = true;
    output.statistics = statistics;
    output.sourceCrs = crsLabelOf(info.projection);
    output.mesh = std::move(mesh);
    if (!info.hasGeoTransform) {
        output.warnings.push_back(
            QStringLiteral("栅格没有地理变换，已按像素坐标显示"));
    } else if (!reprojected && !crsNote.isEmpty()) {
        output.warnings.push_back(crsNote + QStringLiteral("，栅格按源坐标显示"));
    }
    if (originWasZero && (origin[0] != 0.0 || origin[1] != 0.0)) {
        output.warnings.push_back(
            QStringLiteral("为保证渲染精度，已自动以栅格中心作为局部原点"));
    }
    return output;
}

} // namespace View3DModel

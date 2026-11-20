#include "SystemConfig.h"

#include <QColor>
#include <QDateTime>
#include <QFile>
#include <QXmlStreamReader>

#include <cmath>

namespace View3D {
namespace {

bool xmlBool(const QString& value, bool fallback)
{
    if (value.isEmpty())
        return fallback;
    return value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
        value == QStringLiteral("1");
}

double xmlNumber(const QString& value, double fallback)
{
    bool ok = false;
    const double result = value.toDouble(&ok);
    return ok ? result : fallback;
}

void appendWarning(QString* warning, const QString& message)
{
    if (!warning)
        return;
    if (!warning->isEmpty())
        *warning += QLatin1Char('\n');
    *warning += message;
}

bool xmlBoolValidated(const QString& value, bool fallback, QString* warning,
                      const QString& fieldName)
{
    if (value.isEmpty())
        return fallback;
    if (value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
        value == QStringLiteral("1"))
        return true;
    if (value.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0 ||
        value == QStringLiteral("0"))
        return false;
    appendWarning(warning, QStringLiteral("%1必须为 true、false、1 或 0").arg(fieldName));
    return fallback;
}

bool readColour(const QXmlStreamAttributes& attributes, const QString& name,
                const QString& label, float* red, float* green, float* blue,
                QString* warning)
{
    const QString text = attributes.value(name).toString().trimmed();
    if (text.isEmpty())
        return true;
    const QColor colour(text);
    if (!colour.isValid()) {
        appendWarning(warning, QStringLiteral("%1必须是有效颜色").arg(label));
        return false;
    }
    *red = colour.redF();
    *green = colour.greenF();
    *blue = colour.blueF();
    return true;
}

double readFinite(const QXmlStreamAttributes& attributes, const QString& name,
                  double fallback, bool* valid)
{
    const QString text = attributes.value(name).toString().trimmed();
    if (text.isEmpty()) {
        *valid = true;
        return fallback;
    }
    bool ok = false;
    const double value = text.toDouble(&ok);
    *valid = ok && std::isfinite(value);
    return *valid ? value : fallback;
}

} // namespace

SystemConfig SystemConfig::load(const QString& filePath, QString* warning)
{
    SystemConfig config;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendWarning(warning,
                      QStringLiteral("无法读取系统配置，使用内置默认值：%1").arg(filePath));
        return config;
    }

    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        const QStringView tag = xml.name();
        const auto attributes = xml.attributes();
        if (tag == QStringLiteral("viewportcapture")) {
            const QString format =
                attributes.value(QStringLiteral("defaultFormat")).toString().trimmed().toLower();
            if (format == QStringLiteral("png") || format == QStringLiteral("jpg") ||
                format == QStringLiteral("jpeg"))
                config.viewportCapture.defaultFormat =
                    format == QStringLiteral("jpeg") ? QStringLiteral("jpg") : format;
            else if (!format.isEmpty())
                appendWarning(warning, QStringLiteral("截图默认格式仅支持 png 或 jpg"));
            const int quality = static_cast<int>(
                xmlNumber(attributes.value(QStringLiteral("jpegQuality")).toString(),
                          config.viewportCapture.jpegQuality));
            if (quality >= 1 && quality <= 100)
                config.viewportCapture.jpegQuality = quality;
            else
                appendWarning(warning, QStringLiteral("截图 JPEG 质量必须在 1 至 100 之间"));
            const double scale =
                xmlNumber(attributes.value(QStringLiteral("outputScale")).toString(),
                          config.viewportCapture.outputScale);
            if (scale >= 0.25 && scale <= 4.0)
                config.viewportCapture.outputScale = scale;
            else
                appendWarning(warning, QStringLiteral("截图输出倍率必须在 0.25 至 4.0 之间"));
            config.viewportCapture.includeWidgetOverlays = xmlBool(
                attributes.value(QStringLiteral("includeWidgetOverlays")).toString(), false);
        } else if (tag == QStringLiteral("rendering")) {
            ViewportSettings& viewport = config.viewport;
            const QString displayMode =
                attributes.value(QStringLiteral("displayMode")).toString().trimmed().toLower();
            if (displayMode == QStringLiteral("solid"))
                viewport.displayMode = ViewportDisplayMode::Solid;
            else if (displayMode == QStringLiteral("wireframe"))
                viewport.displayMode = ViewportDisplayMode::Wireframe;
            else if (displayMode == QStringLiteral("solid-wireframe"))
                viewport.displayMode = ViewportDisplayMode::SolidWireframe;
            else if (displayMode == QStringLiteral("points"))
                viewport.displayMode = ViewportDisplayMode::Points;
            else if (!displayMode.isEmpty())
                appendWarning(
                    warning,
                    QStringLiteral("显示模式必须为 solid、wireframe、solid-wireframe 或 points"));

            const QString viewMode =
                attributes.value(QStringLiteral("viewMode")).toString().trimmed().toLower();
            if (viewMode == QStringLiteral("2d") || viewMode == QStringLiteral("planar"))
                viewport.planarMode = true;
            else if (viewMode == QStringLiteral("globe") || viewMode == QStringLiteral("sphere"))
                viewport.globeMode = true;
            else if (viewMode == QStringLiteral("3d") || viewMode.isEmpty())
                viewport.planarMode = false;
            else
                appendWarning(warning, QStringLiteral("视图模式必须为 2d、3d 或 globe"));

            const QString backgroundMode =
                attributes.value(QStringLiteral("backgroundMode")).toString().trimmed().toLower();
            if (backgroundMode == QStringLiteral("solid"))
                viewport.background.mode = ViewportBackgroundMode::Solid;
            else if (backgroundMode == QStringLiteral("gradient"))
                viewport.background.mode = ViewportBackgroundMode::Gradient;
            else if (!backgroundMode.isEmpty())
                appendWarning(warning, QStringLiteral("背景模式必须为 solid 或 gradient"));

            readColour(attributes, QStringLiteral("backgroundTopColor"), QStringLiteral("背景顶部颜色"),
                       &viewport.background.topRed, &viewport.background.topGreen,
                       &viewport.background.topBlue, warning);
            readColour(attributes, QStringLiteral("backgroundBottomColor"),
                       QStringLiteral("背景底部颜色"), &viewport.background.bottomRed,
                       &viewport.background.bottomGreen, &viewport.background.bottomBlue,
                       warning);

            viewport.environment.gridVisible = xmlBoolValidated(
                attributes.value(QStringLiteral("gridVisible")).toString(),
                viewport.environment.gridVisible, warning, QStringLiteral("地面网格可见性"));
            viewport.environment.axesVisible = xmlBoolValidated(
                attributes.value(QStringLiteral("axesVisible")).toString(),
                viewport.environment.axesVisible, warning, QStringLiteral("坐标轴可见性"));
            readColour(attributes, QStringLiteral("gridColor"), QStringLiteral("地面网格颜色"),
                       &viewport.environment.gridRed, &viewport.environment.gridGreen,
                       &viewport.environment.gridBlue, warning);

            bool valid = false;
            const double gridExtent = readFinite(attributes, QStringLiteral("gridExtent"),
                                                 viewport.environment.gridExtent, &valid);
            if (valid && gridExtent >= 1.0 && gridExtent <= 10000000.0)
                viewport.environment.gridExtent = static_cast<float>(gridExtent);
            else
                appendWarning(warning, QStringLiteral("地面网格范围必须在 1 至 10000000 之间"));
            const int gridDivisions = static_cast<int>(readFinite(
                attributes, QStringLiteral("gridDivisions"),
                viewport.environment.gridDivisions, &valid));
            if (valid && gridDivisions >= 2 && gridDivisions <= 1000)
                viewport.environment.gridDivisions =
                    static_cast<std::uint32_t>(gridDivisions);
            else
                appendWarning(warning, QStringLiteral("地面网格分割数必须在 2 至 1000 之间"));
            const double axisLength = readFinite(attributes, QStringLiteral("axisLength"),
                                                 viewport.environment.axisLength, &valid);
            if (valid && axisLength >= 0.1 && axisLength <= 10000000.0)
                viewport.environment.axisLength = static_cast<float>(axisLength);
            else
                appendWarning(warning, QStringLiteral("坐标轴长度必须在 0.1 至 10000000 之间"));

            viewport.lighting.ambientVisible = xmlBoolValidated(
                attributes.value(QStringLiteral("ambientLightVisible")).toString(),
                viewport.lighting.ambientVisible, warning, QStringLiteral("环境光可见性"));
            readColour(attributes, QStringLiteral("ambientLightColor"),
                       QStringLiteral("环境光颜色"), &viewport.lighting.ambientRed,
                       &viewport.lighting.ambientGreen, &viewport.lighting.ambientBlue,
                       warning);
            const double ambientIntensity = readFinite(
                attributes, QStringLiteral("ambientLightIntensity"),
                viewport.lighting.ambientIntensity, &valid);
            if (valid && ambientIntensity >= 0.0 && ambientIntensity <= 100.0)
                viewport.lighting.ambientIntensity = static_cast<float>(ambientIntensity);
            else
                appendWarning(warning, QStringLiteral("环境光强度必须是 0 到 100 的有限数值"));

            viewport.lighting.mainLightVisible = xmlBoolValidated(
                attributes.value(QStringLiteral("mainLightVisible")).toString(),
                viewport.lighting.mainLightVisible, warning, QStringLiteral("主方向光可见性"));
            readColour(attributes, QStringLiteral("mainLightColor"),
                       QStringLiteral("主方向光颜色"), &viewport.lighting.mainLightRed,
                       &viewport.lighting.mainLightGreen, &viewport.lighting.mainLightBlue,
                       warning);
            const double mainIntensity = readFinite(
                attributes, QStringLiteral("mainLightIntensity"),
                viewport.lighting.mainLightIntensity, &valid);
            if (valid && mainIntensity >= 0.0 && mainIntensity <= 100.0)
                viewport.lighting.mainLightIntensity = static_cast<float>(mainIntensity);
            else
                appendWarning(warning, QStringLiteral("主方向光强度必须是 0 到 100 的有限数值"));

            bool directionValid = true;
            const double directionX = readFinite(attributes, QStringLiteral("mainLightDirectionX"),
                                                 viewport.lighting.mainLightDirectionX,
                                                 &directionValid);
            const double directionY = readFinite(attributes, QStringLiteral("mainLightDirectionY"),
                                                 viewport.lighting.mainLightDirectionY,
                                                 &directionValid);
            const double directionZ = readFinite(attributes, QStringLiteral("mainLightDirectionZ"),
                                                 viewport.lighting.mainLightDirectionZ,
                                                 &directionValid);
            if (!directionValid)
                appendWarning(warning, QStringLiteral("主方向光方向分量必须是有限数值"));
            else if (directionX * directionX + directionY * directionY +
                         directionZ * directionZ <=
                     1e-12)
                appendWarning(warning, QStringLiteral("主方向光方向不能为零向量"));
            else {
                viewport.lighting.mainLightDirectionX = static_cast<float>(directionX);
                viewport.lighting.mainLightDirectionY = static_cast<float>(directionY);
                viewport.lighting.mainLightDirectionZ = static_cast<float>(directionZ);
            }

            viewport.shadows.enabled = xmlBoolValidated(
                attributes.value(QStringLiteral("shadowEnabled")).toString(),
                viewport.shadows.enabled, warning, QStringLiteral("阴影可见性"));
            const QString shadowQuality =
                attributes.value(QStringLiteral("shadowQuality")).toString().trimmed().toLower();
            if (shadowQuality == QStringLiteral("performance"))
                viewport.shadows.quality = ViewportShadowQuality::Performance;
            else if (shadowQuality == QStringLiteral("balanced"))
                viewport.shadows.quality = ViewportShadowQuality::Balanced;
            else if (shadowQuality == QStringLiteral("quality"))
                viewport.shadows.quality = ViewportShadowQuality::Quality;
            else if (!shadowQuality.isEmpty())
                appendWarning(warning,
                              QStringLiteral("阴影质量必须为 performance、balanced 或 quality"));

            const qulonglong maxVertices = attributes.value(QStringLiteral("maxVertices"))
                                               .toULongLong(&valid);
            if (valid && maxVertices > 0)
                viewport.renderBudgetMaxVertices = maxVertices;
            else if (!attributes.value(QStringLiteral("maxVertices")).toString().isEmpty())
                appendWarning(warning, QStringLiteral("渲染预算 maxVertices 必须是正整数"));

            const double frameRate = readFinite(attributes, QStringLiteral("frameRate"),
                                                viewport.targetFrameRate, &valid);
            if (valid && frameRate >= 1.0 && frameRate <= 240.0)
                viewport.targetFrameRate = frameRate;
            else
                appendWarning(warning, QStringLiteral("目标帧率必须在 1 到 240 之间"));
        } else if (tag == QStringLiteral("tilecache")) {
            TileCacheSettings& cache = config.tileCache;
            const QString preset =
                attributes.value(QStringLiteral("preset")).toString().trimmed().toLower();
            if (preset == QStringLiteral("low") || preset == QStringLiteral("balanced") ||
                preset == QStringLiteral("high"))
                cache = TileCacheSettings::forPreset(preset);
            else if (!preset.isEmpty() && preset != QStringLiteral("custom"))
                appendWarning(warning,
                              QStringLiteral("瓦片缓存档位仅支持 low、balanced、high、custom"));

            // 显式写出的属性覆盖档位基线；单位 MB，解析后换算成字节。
            const auto megabytes = [&attributes, &warning](const QString& name,
                                                           std::uint64_t current) {
                const QString text = attributes.value(name).toString().trimmed();
                if (text.isEmpty())
                    return current;
                bool ok = false;
                const double value = text.toDouble(&ok);
                if (!ok || value <= 0.0) {
                    appendWarning(warning, QStringLiteral("%1 必须是正数（单位 MB）").arg(name));
                    return current;
                }
                return static_cast<std::uint64_t>(value * 1024.0 * 1024.0);
            };
            const auto integer = [&attributes, &warning](const QString& name, int current,
                                                         int minimum, int maximum) {
                const QString text = attributes.value(name).toString().trimmed();
                if (text.isEmpty())
                    return current;
                bool ok = false;
                const double value = text.toDouble(&ok);
                if (!ok || value < minimum || value > maximum) {
                    appendWarning(warning, QStringLiteral("%1 必须在 %2 到 %3 之间")
                                               .arg(name)
                                               .arg(minimum)
                                               .arg(maximum));
                    return current;
                }
                return static_cast<int>(value);
            };
            cache.cacheBytes = megabytes(QStringLiteral("cacheMB"), cache.cacheBytes);
            cache.overflowBytes =
                megabytes(QStringLiteral("overflowMB"), cache.overflowBytes);
            cache.indexCacheBytes =
                megabytes(QStringLiteral("indexCacheMB"), cache.indexCacheBytes);
            cache.maxTiles = static_cast<std::uint32_t>(
                integer(QStringLiteral("maxTiles"), static_cast<int>(cache.maxTiles), 64, 20000));
            cache.screenSpaceErrorPixels =
                integer(QStringLiteral("screenSpaceError"),
                        static_cast<int>(cache.screenSpaceErrorPixels), 2, 128);
            const double centerCone =
                xmlNumber(attributes.value(QStringLiteral("centerCone")).toString(),
                          cache.centerConeFactor);
            if (centerCone >= 0.05 && centerCone <= 1.0)
                cache.centerConeFactor = centerCone;
            else
                appendWarning(warning,
                              QStringLiteral("中心优先锥系数必须在 0.05 到 1.0 之间"));
            cache.workers = integer(QStringLiteral("workers"), cache.workers, 1, 16);
            cache.maxInFlight =
                integer(QStringLiteral("maxInFlight"), cache.maxInFlight, 1, 64);
            cache.maxUploadPending = static_cast<std::uint32_t>(
                integer(QStringLiteral("maxUploadPending"),
                        static_cast<int>(cache.maxUploadPending), 8, 2048));
            cache.keepAliveMs = integer(QStringLiteral("keepAliveMs"), cache.keepAliveMs, 0, 30000);
            const QString siblings =
                attributes.value(QStringLiteral("preloadSiblings")).toString().trimmed();
            if (!siblings.isEmpty())
                cache.preloadSiblings = xmlBool(siblings, cache.preloadSiblings);
            const QString dse =
                attributes.value(QStringLiteral("dynamicScreenSpaceError")).toString().trimmed();
            if (!dse.isEmpty())
                cache.dynamicScreenSpaceError = xmlBool(dse, cache.dynamicScreenSpaceError);
            const double dseFactor =
                xmlNumber(attributes.value(QStringLiteral("dseFactor")).toString(),
                          cache.dseFactor);
            if (dseFactor >= 0.0 && dseFactor <= 32.0)
                cache.dseFactor = dseFactor;
            else if (!attributes.value(QStringLiteral("dseFactor")).toString().isEmpty())
                appendWarning(warning, QStringLiteral("dseFactor 必须在 0 到 32 之间"));
            const double falloff =
                xmlNumber(attributes.value(QStringLiteral("dseHeightFalloff")).toString(),
                          cache.dseHeightFalloff);
            if (falloff >= 0.01 && falloff <= 1.0)
                cache.dseHeightFalloff = falloff;
            else if (!attributes.value(QStringLiteral("dseHeightFalloff")).toString().isEmpty())
                appendWarning(warning, QStringLiteral("dseHeightFalloff 必须在 0.01 到 1.0 之间"));
            const double density =
                xmlNumber(attributes.value(QStringLiteral("dseDensity")).toString(),
                          cache.dseDensity);
            if (density >= 0.0 && density <= 0.05)
                cache.dseDensity = density;
            else if (!attributes.value(QStringLiteral("dseDensity")).toString().isEmpty())
                appendWarning(warning, QStringLiteral("dseDensity 必须在 0 到 0.05 之间"));
            cache.preset = preset.isEmpty() ? QStringLiteral("balanced") : preset;
            cache.clampToValidRange();
        }
    }
    if (xml.hasError()) {
        const QString backup = filePath + QStringLiteral(".corrupt-") +
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        QFile::copy(filePath, backup);
        appendWarning(
            warning,
            QStringLiteral("系统配置 XML 解析失败（第 %1 行）：%2；使用已读取值和内置默认值，"
                           "已备份损坏文件到 %3")
                .arg(xml.lineNumber())
                .arg(xml.errorString())
                .arg(backup));
    }
    return config;
}

} // namespace View3D

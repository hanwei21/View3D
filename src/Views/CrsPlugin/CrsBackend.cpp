#include "CrsBackend.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>
#include <cstring>
#include <vector>

#ifdef VIEW3D_CRS_HAS_PROJ
#include <proj.h>
#endif

namespace View3DCrs {

namespace {

// MSVC 的 <cmath> 默认不暴露 M_PI（需要 _USE_MATH_DEFINES），这里自带常量。
constexpr double kPi = 3.14159265358979323846;
// 主参照系：WGS84 经纬度 + 椭球高。显示投影切换不动它，只动局部轴的取向。
constexpr const char* kMainCrs = "EPSG:4979";

bool looksGeographic(const QString& crs)
{
    const QString upper = crs.toUpper();
    return upper.contains(QStringLiteral("4326")) || upper.contains(QStringLiteral("4979")) ||
        upper.contains(QStringLiteral("CRS84"));
}

bool looksWebMercator(const QString& crs)
{
    const QString upper = crs.toUpper();
    return upper.contains(QStringLiteral("3857")) ||
        upper.contains(QStringLiteral("900913")) ||
        upper.contains(QStringLiteral("PSEUDO-MERCATOR"));
}

// 没有 PROJ 时的兜底：Web Mercator（球面）-> 经纬度，其余源交给调用方告警。
bool fallbackMercatorToGeographic(double* xyz, std::size_t count)
{
    constexpr double kEarthRadius = 6378137.0;
    for (std::size_t i = 0; i < count; ++i) {
        double* point = xyz + i * 3;
        const double x = point[0];
        const double y = point[1];
        if (!std::isfinite(x) || !std::isfinite(y) ||
            std::abs(x) > kEarthRadius * kPi + 1.0 ||
            std::abs(y) > kEarthRadius * kPi + 1.0)
            return false;
        point[0] = x / kEarthRadius * 180.0;
        point[1] = (2.0 * std::atan(std::exp(y / kEarthRadius)) - kPi / 2.0) * 180.0 / kPi;
        // 投影系的 z 当作高程，缺省 0。
        point[2] = std::isfinite(point[2]) ? point[2] : 0.0;
    }
    return true;
}

// 反方向兜底：经纬度 -> Web Mercator，供「显示投影 = 3857」在无 PROJ 时仍可用。
bool fallbackGeographicToMercator(double* xyz, std::size_t count)
{
    constexpr double kEarthRadius = 6378137.0;
    for (std::size_t i = 0; i < count; ++i) {
        double* point = xyz + i * 3;
        const double lon = point[0];
        const double lat = point[1];
        if (!std::isfinite(lon) || !std::isfinite(lat) || std::abs(lat) > 85.05112878)
            return false;
        const double y = std::log(std::tan(kPi / 4.0 + lat * kPi / 360.0)) * kEarthRadius;
        point[0] = lon * kPi / 180.0 * kEarthRadius;
        point[1] = y;
        point[2] = std::isfinite(point[2]) ? point[2] : 0.0;
    }
    return true;
}

#ifdef VIEW3D_CRS_HAS_PROJ
// WKT 里只有坐标系对象才能当参照系用（给别的 GIS 导入、或存回库文件）。
// CONVERSION[…]、DERIVINGCONVERSION[…] 只是投影参数片段，不是完整参照系定义。
bool isCrsWkt(const QString& text)
{
    static const QLatin1String keywords[] = { QLatin1String("PROJCS"),  QLatin1String("GEOGCS"),
                                              QLatin1String("PROJCRS"), QLatin1String("GEOGCRS"),
                                              QLatin1String("BOUNDCRS"), QLatin1String("GEODCRS"),
                                              QLatin1String("DYNAMICCRS"),
                                              QLatin1String("COMPOUNDCRS"), QLatin1String("VERTCRS") };
    for (const QLatin1String& keyword : keywords) {
        if (text.startsWith(keyword))
            return true;
    }
    return false;
}

// EPSG/ESRI 代码、urn、proj 字符串都由 proj_create 识别，认不出再按 WKT 试一次。
PJ* crsFromText(PJ_CONTEXT* context, const QString& crs)
{
    if (crs.trimmed().isEmpty())
        return nullptr;
    const QByteArray utf8 = crs.trimmed().toUtf8();
    PJ* parsed = proj_create(context, utf8.constData());
    if (!parsed) {
        QByteArray wkt = crs.toUtf8();
        // PROJ 9.8 的 proj_create_from_wkt(ctx, wkt, options, availability,
        // recommended_operations)：没有错误出参，返回空即失败。
        parsed = proj_create_from_wkt(context, wkt.constData(), nullptr, nullptr, nullptr);
    }
    return parsed;
}

// 裸 proj 字符串（+proj=tmerc …）在 PROJ 里是 OtherCRS：既不能当 crs_to_crs 的端点，
// 导出的 WKT2 也只是 CONVERSION 片段。按 PROJ 的正规用法把它包进一条 pipeline 才能用。
bool isBareProjectionText(const QString& text)
{
    return text.trimmed().startsWith(QLatin1Char('+'));
}

// 这类管线的地理端约定是弧度而不是角度，所以要把换算方向记下来随管线缓存。
struct PipelinePlan {
    QString text;
    bool radiansFromSource = false;
    bool radiansToTarget = false;
};

PipelinePlan planFor(const QString& source, const QString& target)
{
    PipelinePlan plan;
    const bool sourceBare = isBareProjectionText(source);
    const bool targetBare = isBareProjectionText(target);
    if (sourceBare == targetBare)
        return plan;   // 两端都有权威定义（或都没有）：交给 proj_create_crs_to_crs
    const QString longlat = QStringLiteral("+proj=longlat +ellps=WGS84 +no_defs");
    if (targetBare) {
        plan.text = QStringLiteral("+proj=pipeline +step +inv ") + longlat +
            QStringLiteral(" +step ") + target.trimmed();
        plan.radiansFromSource = true;
    } else {
        plan.text = QStringLiteral("+proj=pipeline +step ") + source.trimmed() +
            QStringLiteral(" +step ") + longlat;
        plan.radiansToTarget = true;
    }
    return plan;
}

// 类型说不出单位是度还是米时（OTHER_CRS），拿一个已知点真变换一次定性质：
// 结果落在千米量级就是平面米（投影系），还在 180/90 内就是度（地理系）；
// 变换建不起来说明这条定义根本不能用，返回 false。调用方必须已持有 m_mutex。
bool probeAxisUnits(PJ_CONTEXT* context, const QString& crsText, PJ* crs, bool* projected,
                    bool* geographic)
{
    if (projected)
        *projected = false;
    if (geographic)
        *geographic = false;
    PJ* transform = nullptr;
    bool radiansInput = false;
    if (isBareProjectionText(crsText)) {
        const PipelinePlan plan = planFor(QStringLiteral("EPSG:4326"), crsText);
        QByteArray utf8 = plan.text.toUtf8();
        transform = proj_create(context, utf8.constData());
        radiansInput = plan.radiansFromSource;
    } else {
        PJ* base = proj_create(context, "EPSG:4326");
        transform = base && crs
            ? proj_create_crs_to_crs_from_pj(context, base, crs, nullptr, nullptr)
            : nullptr;
        if (base)
            proj_destroy(base);
        if (!transform)
            return false;
        PJ* normalized = proj_normalize_for_visualization(context, transform);
        if (normalized) {
            proj_destroy(transform);
            transform = normalized;
        }
    }
    if (!transform)
        return false;
    double coordinates[3] = {117.0, 30.0, 0.0};
    if (radiansInput) {
        coordinates[0] *= kPi / 180.0;
        coordinates[1] *= kPi / 180.0;
    }
    const std::size_t pointSize = sizeof(double) * 3;
    const std::size_t handled = proj_trans_generic(
        transform, PJ_FWD, coordinates, pointSize, 1, coordinates + 1, pointSize, 1,
        coordinates + 2, pointSize, 1, nullptr, 0, 0);
    const bool ok = handled == 1 && std::isfinite(coordinates[0]) &&
        std::isfinite(coordinates[1]);
    if (ok) {
        const bool meters = std::abs(coordinates[0]) > 1000.0 || std::abs(coordinates[1]) > 1000.0;
        if (projected)
            *projected = meters;
        if (geographic)
            *geographic = !meters;
    }
    proj_destroy(transform);
    return ok;
}
#endif

} // namespace

#ifdef VIEW3D_CRS_HAS_PROJ
CrsBackend::CrsBackend()
{
    m_context = proj_context_create();
    m_hasProj = m_context != nullptr;
}

CrsBackend::~CrsBackend()
{
    if (m_context) {
        for (const auto& [key, entry] : m_pipelines) {
            static_cast<void>(key);
            if (entry.pj)
                proj_destroy(static_cast<PJ*>(entry.pj));
        }
        m_pipelines.clear();
        proj_context_destroy(static_cast<PJ_CONTEXT*>(m_context));
    }
    m_context = nullptr;
}

void CrsBackend::initialize(const QString& projDataDir)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_context || projDataDir.isEmpty())
        return;
    if (!QFileInfo::exists(projDataDir))
        return;
    QByteArray utf8 = projDataDir.toUtf8();
    const char* paths[1] = { utf8.constData() };
    proj_context_set_search_paths(static_cast<PJ_CONTEXT*>(m_context), 1, paths);
    // 基准格网（NTv2/EGM）按搜索路径惰性打开；路径变了要重建管线缓存，
    // 否则切换投影前建好的无格网管线会一直沿用。
    for (const auto& [key, entry] : m_pipelines) {
        static_cast<void>(key);
        if (entry.pj)
            proj_destroy(static_cast<PJ*>(entry.pj));
    }
    m_pipelines.clear();
}
#else
CrsBackend::CrsBackend() = default;
CrsBackend::~CrsBackend() = default;
void CrsBackend::initialize(const QString&) {}
#endif

QString CrsBackend::lastError() const
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_lastError;
}

bool CrsBackend::transformToMain(const QString& sourceCrs, double* xyz,
                                 std::size_t pointCount) const
{
    return transformBetween(sourceCrs, QString::fromLatin1(kMainCrs), xyz, pointCount);
}

bool CrsBackend::transformBetween(const QString& sourceCrs, const QString& targetCrs,
                                  double* xyz, std::size_t pointCount) const
{
    if (!xyz || pointCount == 0)
        return false;
    const QString source = sourceCrs.trimmed();
    const QString target = targetCrs.trimmed();
#ifdef VIEW3D_CRS_HAS_PROJ
    if (m_hasProj) {
        // 先转进副本：PROJ 逐点部分成功时不能把半转换结果留给调用方。
        std::vector<double> buffer(xyz, xyz + pointCount * 3);
        PJ* transform = nullptr;
        bool radiansFromSource = false;
        bool radiansToTarget = false;
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            auto* context = static_cast<PJ_CONTEXT*>(m_context);
            const auto key = std::make_pair(source, target);
            const auto cached = m_pipelines.find(key);
            if (cached != m_pipelines.end()) {
                transform = static_cast<PJ*>(cached->second.pj);
                radiansFromSource = cached->second.radiansFromSource;
                radiansToTarget = cached->second.radiansToTarget;
            } else {
                const PipelinePlan plan = planFor(source, target);
                Pipeline entry;
                if (!plan.text.isEmpty()) {
                    // 有一端是裸 proj 串：按 PROJ 的 pipeline 约定组一条出来用。
                    QByteArray utf8 = plan.text.toUtf8();
                    entry.pj = proj_create(context, utf8.constData());
                    entry.radiansFromSource = plan.radiansFromSource;
                    entry.radiansToTarget = plan.radiansToTarget;
                } else {
                    PJ* src = crsFromText(context, source);
                    PJ* dst = crsFromText(context, target);
                    PJ* pipeline = src && dst
                        ? proj_create_crs_to_crs_from_pj(context, src, dst, nullptr, nullptr)
                        : nullptr;
                    // GIS 轴序：经纬度按 (经,纬)、投影按 (东,北)，调用方拿到的一直是 xyz。
                    entry.pj =
                        pipeline ? proj_normalize_for_visualization(context, pipeline) : nullptr;
                    if (pipeline)
                        proj_destroy(pipeline);
                    if (dst)
                        proj_destroy(dst);
                    if (src)
                        proj_destroy(src);
                }
                if (!entry.pj) {
                    m_lastError = QStringLiteral("无法建立 %1 -> %2 的参照系变换")
                                      .arg(source.left(40), target.left(40));
                    return false;
                }
                // 缓存要有上界：源参照系可能是整段 WKT，键很长，逐个攒会吃掉内存。
                if (m_pipelines.size() >= 24) {
                    for (const auto& [oldKey, old] : m_pipelines) {
                        static_cast<void>(oldKey);
                        if (old.pj)
                            proj_destroy(static_cast<PJ*>(old.pj));
                    }
                    m_pipelines.clear();
                }
                m_pipelines[key] = entry;
                transform = static_cast<PJ*>(entry.pj);
                radiansFromSource = entry.radiansFromSource;
                radiansToTarget = entry.radiansToTarget;
            }
            if (!transform)
                return false;
            if (radiansFromSource) {
                for (std::size_t i = 0; i < pointCount; ++i) {
                    buffer[i * 3] *= kPi / 180.0;
                    buffer[i * 3 + 1] *= kPi / 180.0;
                }
            }
            double time = 0.0;
            const std::size_t pointSize = sizeof(double) * 3;
            const std::size_t handled = proj_trans_generic(
                transform, PJ_FWD, buffer.data(), pointSize, pointCount,
                buffer.data() + 1, pointSize, pointCount,
                buffer.data() + 2, pointSize, pointCount,
                &time, 0, 0);
            if (handled != pointCount) {
                m_lastError = QStringLiteral("参照系变换有 %1/%2 个点失败")
                                  .arg(pointCount - handled)
                                  .arg(pointCount);
                return false;
            }
            if (radiansToTarget) {
                for (std::size_t i = 0; i < pointCount; ++i) {
                    buffer[i * 3] *= 180.0 / kPi;
                    buffer[i * 3 + 1] *= 180.0 / kPi;
                }
            }
        }
        std::memcpy(xyz, buffer.data(), pointCount * 3 * sizeof(double));
        return true;
    }
#endif
    const bool sourceGeographic = looksGeographic(source) || source.isEmpty();
    const bool targetGeographic = looksGeographic(target) || target.isEmpty();
    if (sourceGeographic && targetGeographic)
        return true;   // 经纬度之间直通：只补高程缺省值
    if (sourceGeographic && looksWebMercator(target))
        return fallbackGeographicToMercator(xyz, pointCount);
    if (looksWebMercator(source) && targetGeographic)
        return fallbackMercatorToGeographic(xyz, pointCount);
    m_lastError = QStringLiteral("没有 PROJ 时不支持 %1 -> %2 的参照系变换")
                      .arg(source.left(40), target.left(40));
    return false;
}

bool CrsBackend::describe(const QString& sourceCrs,
                          View3D::IGeoReference::CrsInfo* info) const
{
    if (!info)
        return false;
#ifdef VIEW3D_CRS_HAS_PROJ
    if (m_hasProj) {
        std::lock_guard<std::mutex> guard(m_mutex);
        auto* context = static_cast<PJ_CONTEXT*>(m_context);
        PJ* crs = crsFromText(context, sourceCrs);
        if (!crs) {
            m_lastError = QStringLiteral("无法识别源参照系");
            return false;
        }
        const PJ_TYPE type = proj_get_type(crs);
        info->code = sourceCrs.trimmed();
        const char* name = proj_get_name(crs);
        info->name = name ? QString::fromUtf8(name) : QString();
        info->geographic = type == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
            type == PJ_TYPE_GEOGRAPHIC_3D_CRS || type == PJ_TYPE_GEOGRAPHIC_CRS;
        info->projected = type == PJ_TYPE_PROJECTED_CRS ||
            type == PJ_TYPE_DERIVED_PROJECTED_CRS;
        // 这版 PROJ 的枚举里没有 Cartesian CS 类型，投影/地心系按直角坐标处理。
        info->cartesian = info->projected || type == PJ_TYPE_GEOCENTRIC_CRS;
        if (!info->geographic && !info->projected && type != PJ_TYPE_GEOCENTRIC_CRS) {
            bool probeProjected = false;
            bool probeGeographic = false;
            if (!probeAxisUnits(context, sourceCrs, crs, &probeProjected, &probeGeographic)) {
                m_lastError = QStringLiteral("参照系定义无法建立变换：%1")
                                  .arg(sourceCrs.trimmed().left(60));
                proj_destroy(crs);
                return false;
            }
            info->geographic = probeGeographic;
            info->projected = probeProjected;
            info->cartesian = probeProjected;
        }
        proj_destroy(crs);
        return true;
    }
#endif
    info->code = sourceCrs.trimmed();
    info->geographic = looksGeographic(sourceCrs) || sourceCrs.isEmpty();
    info->projected = looksWebMercator(sourceCrs);
    info->cartesian = info->projected;
    info->name = info->geographic ? QStringLiteral("WGS 84 (经纬度)")
                                  : (info->projected ? QStringLiteral("WGS 84 / Pseudo-Mercator")
                                                     : QString());
    return info->geographic || info->projected;
}

QString CrsBackend::labelOf(const QString& crsText, const QString& name)
{
    static const QRegularExpression kAuthorityCode(
        QStringLiteral("^(EPSG|ESRI|IAU|OGC|URN):[A-Za-z0-9:_-]+$"));
    const QString trimmed = crsText.trimmed();
    if (kAuthorityCode.match(trimmed).hasMatch())
        return trimmed;
    if (!name.isEmpty())
        return name;
    return QStringLiteral("未命名参照系");
}

bool CrsBackend::inspect(const QString& crsText, CrsDefinition* out) const
{
    if (!out || crsText.trimmed().isEmpty())
        return false;
    *out = CrsDefinition {};
#ifdef VIEW3D_CRS_HAS_PROJ
    if (m_hasProj) {
        std::lock_guard<std::mutex> guard(m_mutex);
        auto* context = static_cast<PJ_CONTEXT*>(m_context);
        PJ* crs = crsFromText(context, crsText);
        if (!crs) {
            m_lastError = QStringLiteral("无法识别参照系定义：%1")
                              .arg(crsText.left(60).replace(QLatin1Char('\n'), QLatin1Char(' ')));
            return false;
        }
        const PJ_TYPE type = proj_get_type(crs);
        out->geographic = type == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
            type == PJ_TYPE_GEOGRAPHIC_3D_CRS || type == PJ_TYPE_GEOGRAPHIC_CRS;
        out->projected = type == PJ_TYPE_PROJECTED_CRS || type == PJ_TYPE_DERIVED_PROJECTED_CRS;
        if (!out->geographic && !out->projected && type != PJ_TYPE_GEOCENTRIC_CRS) {
            // 纯 proj 串会被建成 OTHER_CRS，和 describe() 一样靠试变换定性质。
            probeAxisUnits(context, crsText, crs, &out->projected, &out->geographic);
        }
        out->kind = out->geographic ? QStringLiteral("地理参考系")
                                    : (out->projected ? QStringLiteral("投影参考系")
                                                      : (type == PJ_TYPE_GEOCENTRIC_CRS
                                                             ? QStringLiteral("地心直角坐标")
                                                             : QStringLiteral("其他参考系")));
        if (const char* name = proj_get_name(crs))
            out->name = QString::fromUtf8(name);
        const char* authority = proj_get_id_auth_name(crs, 0);
        const char* authorityCode = proj_get_id_code(crs, 0);
        if (authority && authorityCode)
            out->code = QStringLiteral("%1:%2")
                            .arg(QString::fromUtf8(authority).toUpper(),
                                 QString::fromUtf8(authorityCode));
        // 本工程的 PROJ 头文件把这两个导出函数声明成 const char*（字符串归对象持有，
        // 随 PJ 一起释放），所以只取值、不销毁。
        if (const char* text = proj_as_proj_string(context, crs, PJ_PROJ_5, nullptr))
            out->projString = QString::fromUtf8(text);
        // 纯 proj 字符串定义的坐标系导 WKT2 可能失败（PROJ 没有等价节点），
        // 导不出就只留 proj 串，库里两个字段谁有值用谁。裸串常被解析成
        // CONVERSION[…] 片段：那不是参照系文本，存进库后导出的 .prj 在 GIS 里读不懂。
        if (const char* text = proj_as_wkt(context, crs, PJ_WKT2_2019, nullptr)) {
            const QString candidate = QString::fromUtf8(text);
            if (isCrsWkt(candidate))
                out->wkt = candidate;
        }
        proj_destroy(crs);
        if (out->name.isEmpty())
            out->name = out->code;
        return true;
    }
#endif
    // 无 PROJ：describe 自己会加锁，这里不能在它的临界区里调用。
    View3D::IGeoReference::CrsInfo info;
    if (!describe(crsText, &info) || (!info.geographic && !info.projected))
        return false;
    out->name = info.name;
    out->code = info.code;
    out->geographic = info.geographic;
    out->projected = info.projected;
    out->kind = info.geographic ? QStringLiteral("地理参考系") : QStringLiteral("投影参考系");
    return true;
}

// proj 串里的 +proj=xxx 简称翻成看得懂的方法名。权威库自带方法名，这里只给
// 「自定义」里手工录入的裸串用，认不出来就原样显示简称。
QString methodNameOfAcronym(const QString& acronym)
{
    static const struct {
        const char* acronym;
        const char* label;
    } kNames[] = { { "tmerc", "横轴墨卡托（高斯-克吕格）" },
                   { "utm", "通用横轴墨卡托（UTM）" },
                   { "merc", "墨卡托" },
                   { "webmerc", "伪墨卡托（Web Mercator）" },
                   { "lcc", "兰勃特等角圆锥" },
                   { "aea", "阿尔伯斯等积圆锥" },
                   { "stere", "球面投影" },
                   { "somerc", "斜球面墨卡托" },
                   { "omerc", "斜墨卡托（HOTINE）" },
                   { "krovak", "克瓦罗克" },
                   { "laea", "兰勃特等积方位" },
                   { "aqd", "赤经差" },
                   { "geos", "地球同步" },
                   { "longlat", "经纬度（地理系）" },
                   { "eqc", "等距圆柱" } };
    for (const auto& entry : kNames) {
        if (acronym.compare(QLatin1String(entry.acronym), Qt::CaseInsensitive) == 0)
            return QString::fromUtf8(entry.label);
    }
    return acronym.isEmpty() ? QString() : QStringLiteral("%1（PROJ 简称）").arg(acronym);
}

QString projectionMethodFromProjString(const QString& projString)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(\+proj=([A-Za-z0-9_\-]+))"));
    const QRegularExpressionMatch match = pattern.match(projString);
    if (!match.hasMatch())
        return {};
    return methodNameOfAcronym(match.captured(1));
}

bool CrsBackend::detail(const QString& crsText, CrsDetail* out) const
{
    if (!out || crsText.trimmed().isEmpty())
        return false;
    *out = CrsDetail {};
#ifdef VIEW3D_CRS_HAS_PROJ
    if (!m_hasProj) {
        m_lastError = QStringLiteral("PROJ 不可用，读不出参照系详情");
        return false;
    }
    std::lock_guard<std::mutex> guard(m_mutex);
    auto* context = static_cast<PJ_CONTEXT*>(m_context);
    PJ* crs = crsFromText(context, crsText);
    if (!crs) {
        m_lastError = QStringLiteral("无法识别参照系定义：%1")
                          .arg(crsText.left(60).replace(QLatin1Char('\n'), QLatin1Char(' ')));
        return false;
    }
    const PJ_TYPE type = proj_get_type(crs);
    out->geographic = type == PJ_TYPE_GEOGRAPHIC_2D_CRS || type == PJ_TYPE_GEOGRAPHIC_3D_CRS ||
        type == PJ_TYPE_GEOGRAPHIC_CRS;
    out->projected = type == PJ_TYPE_PROJECTED_CRS || type == PJ_TYPE_DERIVED_PROJECTED_CRS;
    if (!out->geographic && !out->projected && type != PJ_TYPE_GEOCENTRIC_CRS)
        probeAxisUnits(context, crsText, crs, &out->projected, &out->geographic);
    out->kind = out->geographic ? QStringLiteral("地理参考系")
                                : (out->projected ? QStringLiteral("投影参考系")
                                                  : (type == PJ_TYPE_GEOCENTRIC_CRS
                                                         ? QStringLiteral("地心直角坐标")
                                                         : QStringLiteral("其他参考系")));
    if (const char* name = proj_get_name(crs))
        out->name = QString::fromUtf8(name);
    if (const char* authority = proj_get_id_auth_name(crs, 0)) {
        if (const char* code = proj_get_id_code(crs, 0))
            out->code = QStringLiteral("%1:%2")
                            .arg(QString::fromUtf8(authority).toUpper(),
                                 QString::fromUtf8(code));
    }
    out->deprecated = proj_is_deprecated(crs) != 0;
    if (const char* scope = proj_get_scope(crs))
        out->scope = QString::fromUtf8(scope);
    if (const char* remarks = proj_get_remarks(crs))
        out->remarks = QString::fromUtf8(remarks);
    double west = 0.0;
    double south = 0.0;
    double east = 0.0;
    double north = 0.0;
    const char* areaName = nullptr;
    // 返回的范围名由 PJ 对象持有，随它一起释放，所以只取值不释放。
    if (proj_get_area_of_use(context, crs, &west, &south, &east, &north, &areaName)) {
        out->hasArea = true;
        out->west = west;
        out->south = south;
        out->east = east;
        out->north = north;
    }
    if (areaName)
        out->areaName = QString::fromUtf8(areaName);
    if (PJ* ellipsoid = proj_get_ellipsoid(context, crs)) {
        if (const char* name = proj_get_name(ellipsoid))
            out->ellipsoid = QString::fromUtf8(name);
        proj_destroy(ellipsoid);
    }
    if (PJ* datum = proj_crs_get_datum(context, crs)) {
        if (const char* name = proj_get_name(datum))
            out->datum = QString::fromUtf8(name);
        proj_destroy(datum);
    }
    if (const char* text = proj_as_proj_string(context, crs, PJ_PROJ_5, nullptr))
        out->projString = QString::fromUtf8(text);
    if (const char* text = proj_as_wkt(context, crs, PJ_WKT2_2019, nullptr)) {
        const QString candidate = QString::fromUtf8(text);
        if (isCrsWkt(candidate))
            out->wkt = candidate;
    }
    proj_destroy(crs);
    out->projectionMethod = projectionMethodFromProjString(out->projString);
    if (out->name.isEmpty())
        out->name = out->code;
    return true;
#else
    m_lastError = QStringLiteral("本工程没带 PROJ，读不出参照系详情");
    static_cast<void>(crsText);
    return false;
#endif
}

bool CrsBackend::isDisplayable(const QString& crs, bool* projected) const
{
    View3D::IGeoReference::CrsInfo info;
    if (!describe(crs, &info))
        return false;
    if (projected)
        *projected = info.projected;
    // 地心直角系没有平面轴，能识别也不能当显示参照系。
    return info.geographic || info.projected;
}

QStringList CrsBackend::authorities() const
{
    QStringList result;
#ifdef VIEW3D_CRS_HAS_PROJ
    if (!m_hasProj)
        return result;
    std::lock_guard<std::mutex> guard(m_mutex);
    auto* context = static_cast<PJ_CONTEXT*>(m_context);
    PROJ_STRING_LIST list = proj_get_authorities_from_database(context);
    for (PROJ_STRING_LIST item = list; item && *item; ++item)
        result.append(QString::fromUtf8(*item));
    proj_string_list_destroy(list);
#endif
    return result;
}

bool CrsBackend::listAuthorityCrs(const QString& authName, bool allowDeprecated,
                                  std::vector<CrsSummary>* out) const
{
    if (!out)
        return false;
    out->clear();
#ifdef VIEW3D_CRS_HAS_PROJ
    if (!m_hasProj) {
        m_lastError = QStringLiteral("PROJ 不可用，读不了参照系权威库");
        return false;
    }
    std::lock_guard<std::mutex> guard(m_mutex);
    auto* context = static_cast<PJ_CONTEXT*>(m_context);
    // 只要地理系和投影系：垂直系/复合系没有可用的平面轴，地心系同理，
    // 混进列表里只会让人选了不能用的那条。
    static const PJ_TYPE kTypes[] = { PJ_TYPE_GEOGRAPHIC_2D_CRS, PJ_TYPE_GEOGRAPHIC_3D_CRS,
                                      PJ_TYPE_PROJECTED_CRS };
    PROJ_CRS_LIST_PARAMETERS* params = proj_get_crs_list_parameters_create();
    if (!params) {
        m_lastError = QStringLiteral("参照系库查询参数创建失败");
        return false;
    }
    params->types = kTypes;
    params->typesCount = sizeof(kTypes) / sizeof(kTypes[0]);
    params->allow_deprecated = allowDeprecated ? 1 : 0;
    // 只看地球：EPSG/ESRI 里带火星、月球坐标系（IAU），对这个工程没用。
    params->celestial_body_name = "Earth";
    int count = 0;
    const QByteArray utf8 = authName.toUtf8();
    PROJ_CRS_INFO** list =
        proj_get_crs_info_list_from_database(context, utf8.constData(), params, &count);
    proj_get_crs_list_parameters_destroy(params);
    if (!list) {
        m_lastError = QStringLiteral("参照系权威库读不出来（proj.db 缺失或版本不符）");
        return false;
    }
    out->reserve(static_cast<std::size_t>(count > 0 ? count : 0));
    for (int index = 0; index < count; ++index) {
        const PROJ_CRS_INFO* info = list[index];
        if (!info)
            continue;
        CrsSummary summary;
        summary.authName = info->auth_name ? QString::fromUtf8(info->auth_name) : authName;
        summary.code = info->code ? QString::fromUtf8(info->code) : QString();
        summary.name = info->name ? QString::fromUtf8(info->name) : QString();
        summary.projectionMethod =
            info->projection_method_name ? QString::fromUtf8(info->projection_method_name)
                                         : QString();
        summary.areaName = info->area_name ? QString::fromUtf8(info->area_name) : QString();
        summary.geographic = info->type == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
            info->type == PJ_TYPE_GEOGRAPHIC_3D_CRS;
        summary.geographic3d = info->type == PJ_TYPE_GEOGRAPHIC_3D_CRS;
        summary.projected = info->type == PJ_TYPE_PROJECTED_CRS;
        summary.deprecated = info->deprecated != 0;
        summary.hasArea = info->bbox_valid != 0;
        summary.west = info->west_lon_degree;
        summary.south = info->south_lat_degree;
        summary.east = info->east_lon_degree;
        summary.north = info->north_lat_degree;
        if (summary.name.isEmpty())
            summary.name = summary.authorityCode();
        out->push_back(std::move(summary));
    }
    proj_crs_info_list_destroy(list);
    return !out->empty();
#else
    m_lastError = QStringLiteral("本工程没带 PROJ，参照系权威库不可用");
    static_cast<void>(authName);
    static_cast<void>(allowDeprecated);
    return false;
#endif
}

} // namespace View3DCrs

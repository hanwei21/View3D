#include "CrsAuthority.h"

#include <QStringList>

#include <algorithm>
#include <utility>

namespace View3DCrs {

namespace {

using Item = View3D::ICrsBrowser::Item;
using Group = View3D::ICrsBrowser::Group;

const char* kGeographic2d = "builtin.geographic2d";
const char* kGeographic3d = "builtin.geographic3d";
const char* kTmUtm = "builtin.tm.utm";
const char* kTmGkZone = "builtin.tm.gk_zone";
const char* kTmGkCm = "builtin.tm.gk_cm";
const char* kTmOther = "builtin.tm.other";
const char* kMethodOther = "builtin.method.other";

// EPSG 的投影方法名 → 中文。表里没有的原样显示，至少不会给出错的翻译。
QString chineseMethod(const QString& method)
{
    static const struct {
        const char* from;
        const char* to;
    } kMap[] = {
        { "Transverse Mercator", "横轴墨卡托" },
        { "Transverse Mercator (South Orientated)", "横轴墨卡托（南朝向）" },
        { "Lambert Conic Conformal (2SP)", "兰勃特等角圆锥（2SP）" },
        { "Lambert Conic Conformal (1SP)", "兰勃特等角圆锥（1SP）" },
        { "Lambert Conic Conformal (West Orientated)", "兰勃特等角圆锥（西朝向）" },
        { "Albers Equal Area", "阿尔伯斯等积圆锥" },
        { "Mercator (1SP)", "墨卡托（1SP）" },
        { "Mercator (2SP)", "墨卡托（2SP）" },
        { "Popular Visualisation Pseudo Mercator", "伪墨卡托（Web Mercator）" },
        { "Polar Stereographic (variant A)", "极区球面（A 式）" },
        { "Polar Stereographic (variant B)", "极区球面（B 式）" },
        { "Polar Stereographic (variant C)", "极区球面（C 式）" },
        { "Oblique Stereographic", "斜球面" },
        { "Hotine Oblique Mercator (variant A)", "霍廷斜墨卡托（A 式）" },
        { "Hotine Oblique Mercator (variant B)", "霍廷斜墨卡托（B 式）" },
        { "Lambert Azimuthal Equal Area", "兰勃特等积方位" },
        { "Cassini-Soldner", "卡索尼-桑德" },
        { "Krovak", "克瓦罗克" },
        { "Equidistant Cylindrical", "等距圆柱" },
        { "Modified American Polyconic", "改良美国圆锥" },
        { "Van der Grinten", "范德格林滕" },
    };
    for (const auto& entry : kMap) {
        if (method == QLatin1String(entry.from))
            return QString::fromUtf8(entry.to);
    }
    return method;
}

// 横轴墨卡托一组按 EPSG 官方名的写法再切一刀：本工程挑参照系就是在挑带号和中央经线，
// 3800 多条混在一格里没法用。UTM 单列也是 QGIS 的同款处理。
void transverseMercatorGroup(const QString& name, QString* id, QString* label)
{
    if (name.contains(QStringLiteral("UTM zone"), Qt::CaseInsensitive)) {
        *id = QLatin1String(kTmUtm);
        *label = QStringLiteral("横轴墨卡托 · UTM 分区");
        return;
    }
    if (name.contains(QStringLiteral("Gauss-Kruger zone"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("Gauss Kruger zone"), Qt::CaseInsensitive)) {
        *id = QLatin1String(kTmGkZone);
        *label = QStringLiteral("高斯-克吕格 · 带号（zone）");
        return;
    }
    if (name.contains(QStringLiteral("Gauss-Kruger CM"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("Gauss Kruger CM"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("Central Meridian"), Qt::CaseInsensitive)) {
        *id = QLatin1String(kTmGkCm);
        *label = QStringLiteral("高斯-克吕格 · 中央经线（CM）");
        return;
    }
    *id = QLatin1String(kTmOther);
    *label = QStringLiteral("横轴墨卡托 · 其它");
}

void classify(const CrsBackend::CrsSummary& summary, const Item& item, QString* id,
              QString* label)
{
    if (summary.projected) {
        if (summary.projectionMethod.compare(QStringLiteral("Transverse Mercator"),
                                             Qt::CaseInsensitive) == 0) {
            transverseMercatorGroup(item.name, id, label);
            return;
        }
        *id = QStringLiteral("builtin.method.") + summary.projectionMethod;
        *label = item.method.isEmpty() ? QStringLiteral("其它投影方法") : item.method;
        return;
    }
    // 3D 与否听 PROJ 的类型（EPSG:4979 官方名也叫 "WGS 84"，名字里看不出 3D）。
    *id = QLatin1String(summary.geographic3d ? kGeographic3d : kGeographic2d);
    *label = summary.geographic3d ? QStringLiteral("地理坐标系（3D 经纬度+椭球高）")
                                  : QStringLiteral("地理坐标系（2D 经纬度）");
}

bool isFixedGroup(const QString& id)
{
    return id.startsWith(QLatin1String("builtin.geographic")) ||
        id.startsWith(QLatin1String("builtin.tm."));
}

// 树里的顺序：2D 地理 → 3D 地理 → 中国最常用的 CM/带号 → UTM → TM 其它 →
// 其余投影方法按条数降序。
int groupRank(const QString& id)
{
    if (id == QLatin1String(kGeographic2d))
        return 0;
    if (id == QLatin1String(kGeographic3d))
        return 1;
    if (id == QLatin1String(kTmGkCm))
        return 2;
    if (id == QLatin1String(kTmGkZone))
        return 3;
    if (id == QLatin1String(kTmUtm))
        return 4;
    if (id == QLatin1String(kTmOther))
        return 5;
    return 6;
}

} // namespace


CrsAuthority::CrsAuthority(const CrsBackend& backend)
    : m_backend(backend), m_status(QStringLiteral("内置参照系还没读（打开面板时才枚举）"))
{
}

void CrsAuthority::load()
{
    if (m_loaded)
        return;
    m_loaded = true;
    if (!m_backend.available()) {
        m_status = QStringLiteral("！本机没有 PROJ 运行库，内置参照系不可用");
        return;
    }
    std::vector<CrsBackend::CrsSummary> summaries;
    if (!m_backend.listAuthorityCrs(QStringLiteral("EPSG"), false, &summaries)) {
        m_status = QStringLiteral("！内置参照系读不出来：%1").arg(m_backend.lastError());
        return;
    }

    QStringList ids;
    QHash<QString, QString> labels;
    QHash<QString, QList<Item>> buckets;
    int geographic = 0;
    int projected = 0;
    for (const CrsBackend::CrsSummary& summary : summaries) {
        Item item;
        item.authority = summary.authName;
        item.code = summary.code;
        item.key = summary.authorityCode();
        item.name = summary.name;
        item.method = chineseMethod(summary.projectionMethod);
        item.areaName = summary.areaName;
        item.projected = summary.projected;
        item.geographic = summary.geographic;
        item.deprecated = summary.deprecated;
        item.hasArea = summary.hasArea;
        item.west = summary.west;
        item.south = summary.south;
        item.east = summary.east;
        item.north = summary.north;
        QString id;
        QString label;
        classify(summary, item, &id, &label);
        if (!buckets.contains(id)) {
            ids.append(id);
            labels.insert(id, label);
        }
        buckets[id].append(item);
        m_byKey.insert(item.key, item);
        if (summary.projected)
            ++projected;
        else
            ++geographic;
    }

    // 只有两三条的方法组并到「其它投影方法」，树里不至于长出二十几个小分支。
    QList<Item> loose;
    for (int index = ids.size() - 1; index >= 0; --index) {
        const QString id = ids.at(index);
        if (isFixedGroup(id) || buckets.value(id).size() >= 5)
            continue;
        loose.append(buckets.take(id));
        labels.remove(id);
        ids.removeAt(index);
    }
    if (!loose.isEmpty()) {
        std::sort(loose.begin(), loose.end(),
                  [](const Item& left, const Item& right) { return left.name < right.name; });
        ids.append(QLatin1String(kMethodOther));
        labels.insert(QLatin1String(kMethodOther), QStringLiteral("其它投影方法"));
        buckets.insert(QLatin1String(kMethodOther), loose);
    }

    std::sort(ids.begin(), ids.end(), [&buckets](const QString& left, const QString& right) {
        const int leftRank = groupRank(left);
        const int rightRank = groupRank(right);
        if (leftRank != rightRank)
            return leftRank < rightRank;
        if (leftRank >= 6)
            return buckets.value(left).size() > buckets.value(right).size();
        return left < right;
    });
    for (const QString& id : std::as_const(ids)) {
        Group group;
        group.id = id;
        group.label = labels.value(id);
        group.items = buckets.value(id);
        m_groups.append(group);
    }
    m_status = QStringLiteral("内置 EPSG 参照系 %1 条（地理 %2 / 投影 %3），分 %4 组")
                   .arg(summaries.size())
                   .arg(geographic)
                   .arg(projected)
                   .arg(m_groups.size());
}

const QList<Group>& CrsAuthority::groups()
{
    load();
    return m_groups;
}

bool CrsAuthority::available() const
{
    return m_backend.available();
}

QString CrsAuthority::statusText() const
{
    return m_status;
}

bool CrsAuthority::item(const QString& key, Item* out) const
{
    auto found = m_byKey.constFind(key);
    if (found == m_byKey.constEnd())
        return false;
    if (out)
        *out = found.value();
    return true;
}

bool CrsAuthority::info(const QString& definition, Info* out) const
{
    if (!out)
        return false;
    *out = Info {};
    const QString trimmed = definition.trimmed();
    if (trimmed.isEmpty()) {
        m_lastError = QStringLiteral("参照系定义为空");
        return false;
    }
    CrsBackend::CrsDetail detail {};
    if (!m_backend.detail(trimmed, &detail)) {
        m_lastError = m_backend.lastError();
        return false;
    }
    out->name = detail.name;
    out->code = detail.code;
    out->kind = detail.kind;
    out->method = detail.projectionMethod;
    out->datum = detail.datum;
    out->ellipsoid = detail.ellipsoid;
    out->scope = detail.scope;
    out->remarks = detail.remarks;
    out->projString = detail.projString;
    out->wkt = detail.wkt;
    out->geographic = detail.geographic;
    out->projected = detail.projected;
    out->deprecated = detail.deprecated;
    out->hasArea = detail.hasArea;
    out->west = detail.west;
    out->south = detail.south;
    out->east = detail.east;
    out->north = detail.north;
    // 内置条目自带「使用范围」名字，比 PROJ 按对象反查的更准。
    Item builtin;
    if (item(trimmed, &builtin)) {
        if (!builtin.areaName.isEmpty())
            out->areaName = builtin.areaName;
        if (builtin.hasArea && !out->hasArea) {
            out->hasArea = true;
            out->west = builtin.west;
            out->south = builtin.south;
            out->east = builtin.east;
            out->north = builtin.north;
        }
    }
    if (out->areaName.isEmpty())
        out->areaName = detail.areaName;
    return true;
}

QString CrsAuthority::lastError() const
{
    return m_lastError;
}

} // namespace View3DCrs

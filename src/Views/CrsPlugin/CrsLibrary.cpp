#include "CrsLibrary.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUuid>

namespace View3DCrs {

// 头文件里条目类型来自契约（ICrsLibrary.h），这里保留旧名字少改一片代码。
using CrsEntry = View3D::CrsLibraryEntry;

namespace {

constexpr const char* kLibraryEnv = "VIEW3D_CRS_LIBRARY";

QString normalizedDefinition(const QString& text)
{
    // 判重比的是「压掉换行与多余空白」后的定义：同一段 WKT 换个缩进不算两个坐标系。
    QString collapsed;
    collapsed.reserve(text.size());
    QChar previous;
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            if (!previous.isSpace() && !collapsed.isEmpty())
                collapsed.append(QLatin1Char(' '));
            continue;
        }
        collapsed.append(ch);
        previous = ch;
    }
    return collapsed.trimmed();
}

QJsonObject entryToJson(const CrsEntry& entry)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), entry.id);
    object.insert(QStringLiteral("name"), entry.name);
    object.insert(QStringLiteral("definition"), entry.definition);
    object.insert(QStringLiteral("code"), entry.code);
    object.insert(QStringLiteral("projString"), entry.projString);
    object.insert(QStringLiteral("wkt"), entry.wkt);
    object.insert(QStringLiteral("note"), entry.note);
    object.insert(QStringLiteral("origin"), entry.origin);
    return object;
}

QString definitionOfObject(const QJsonObject& object)
{
    // 兼容三种写法：显式 definition、只有 WKT（ESRI/PRJ 风格）、只有 proj 串。
    const QString definition = object.value(QStringLiteral("definition")).toString().trimmed();
    if (!definition.isEmpty())
        return definition;
    const QString wkt = object.value(QStringLiteral("wkt")).toString().trimmed();
    if (!wkt.isEmpty())
        return wkt;
    return object.value(QStringLiteral("projString")).toString().trimmed();
}

} // namespace

CrsLibrary::CrsLibrary(const CrsBackend& backend)
    : m_backend(backend)
{
}

QString CrsLibrary::filePath() const
{
    const QString fromEnv = qEnvironmentVariable(kLibraryEnv);
    if (!fromEnv.trimmed().isEmpty())
        return QDir::cleanPath(fromEnv.trimmed());
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("crs_library.json"));
}

bool CrsLibrary::writeJson(const QString& path, const QList<CrsEntry>& entries,
                           const QStringList& favorites, QString* error) const
{
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    QJsonArray array;
    for (const CrsEntry& entry : entries)
        array.append(entryToJson(entry));
    QJsonObject root;
    // version 2 起多一个 favorites 数组。读的时候不看版本号：缺字段就用默认常用集。
    root.insert(QStringLiteral("version"), 2);
    root.insert(QStringLiteral("crs"), array);
    root.insert(QStringLiteral("favorites"), QJsonArray::fromStringList(favorites));

    QFileInfo info(path);
    if (info.absoluteDir().exists() == false &&
        !QDir().mkpath(info.absolutePath()))
        return fail(QStringLiteral("无法创建目录：%1").arg(info.absolutePath()));
    // 先写同目录临时文件再替换：直接覆盖写中途失败会把手上的库截断成半个 JSON。
    const QString temporary = path + QStringLiteral(".tmp");
    {
        QFile file(temporary);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(QStringLiteral("无法写入：%1（%2）").arg(temporary, file.errorString()));
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
    }
    if (QFile::exists(path) && !QFile::remove(path)) {
        QFile::remove(temporary);
        return fail(QStringLiteral("无法替换旧库文件：%1（文件可能被占用）").arg(path));
    }
    if (!QFile::rename(temporary, path)) {
        QFile::remove(temporary);
        return fail(QStringLiteral("无法保存参照系列：%1").arg(path));
    }
    return true;
}

bool CrsLibrary::load(QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    const QString path = filePath();
    QFile file(path);
    if (!QFileInfo::exists(path)) {
        m_entries.clear();
        m_favorites = defaultFavorites();
        m_dirty = false;
        return true;   // 没有库是正常状态：首次保存时才创建
    }
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法读取参照系列：%1（%2）").arg(path, file.errorString()));
    const QByteArray raw = file.readAll();
    file.close();
    QJsonParseError parseError {};
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
    if (!document.isObject())
        return fail(QStringLiteral("参照系列格式错误：%1（%2）").arg(path, parseError.errorString()));

    QList<CrsEntry> loaded;
    int skipped = 0;
    const QJsonArray array = document.object().value(QStringLiteral("crs")).toArray();
    for (const QJsonValue& value : array) {
        if (!value.isObject()) {
            ++skipped;
            continue;
        }
        const QJsonObject object = value.toObject();
        const QString definition = definitionOfObject(object);
        CrsEntry entry;
        entry.name = object.value(QStringLiteral("name")).toString();
        entry.note = object.value(QStringLiteral("note")).toString();
        entry.origin = object.value(QStringLiteral("origin")).toString();
        QString resolveError;
        // 重新解析一遍：换台机器、PROJ 数据版本不同或定义被手改坏时，
        // 坏条目不该把整库拖垮，也不能让它进到能切投影的下拉里。
        if (definition.isEmpty() ||
            !addWithOrigin(entry.name, definition, entry.note, entry.origin, &entry,
                           &resolveError)) {
            ++skipped;
            continue;
        }
        // 沿用文件里的标识：编辑与删除都按 id 定位，重启不该给同一条换号。
        // 撞上重复 id（手改过的库）就保留上面新生成的那个。
        const QString storedId = object.value(QStringLiteral("id")).toString();
        bool idTaken = false;
        for (const CrsEntry& existing : loaded)
            idTaken = idTaken || existing.id == storedId;
        if (!storedId.isEmpty() && !idTaken)
            entry.id = storedId;
        loaded.append(entry);
    }
    m_entries = loaded;
    // 收藏：没有这个字段（老库文件）就用默认常用集；有就照文件走。
    const QJsonArray storedFavorites =
        document.object().value(QStringLiteral("favorites")).toArray();
    if (document.object().contains(QStringLiteral("favorites"))) {
        QStringList favorites;
        for (const QJsonValue& value : storedFavorites) {
            const QString definition = value.toString().trimmed();
            if (definition.isEmpty())
                continue;
            // 认不出来的收藏（换机器、码被删）直接丢：下拉里留一条选了就报错的
            // 项，比少一项更难解释。
            CrsBackend::CrsDefinition parsed;
            if (m_backend.inspect(definition, &parsed))
                favorites.append(definition);
        }
        m_favorites = favorites;
    } else {
        m_favorites = defaultFavorites();
    }
    m_dirty = false;
    if (skipped > 0 && error)
        *error = QStringLiteral("已忽略 %1 条无法识别的参照系定义").arg(skipped);
    return true;
}

bool CrsLibrary::save(QString* error)
{
    if (!writeJson(filePath(), m_entries, m_favorites, error))
        return false;
    m_dirty = false;
    return true;
}

QStringList CrsLibrary::defaultFavorites()
{
    // 开箱就能用的那几条：WGS84 / CGCS2000 经纬度、主系、Web Mercator、
    // UTM 50N/51N、CGCS2000 3°带（中央经线写法与带号写法各一条）。
    return { QStringLiteral("EPSG:4326"), QStringLiteral("EPSG:4490"),
             QStringLiteral("EPSG:4979"), QStringLiteral("EPSG:3857"),
             QStringLiteral("EPSG:32650"), QStringLiteral("EPSG:32651"),
             QStringLiteral("EPSG:4547"), QStringLiteral("EPSG:4526") };
}

bool CrsLibrary::isFavorite(const QString& definition) const
{
    return m_favorites.contains(definition.trimmed());
}

void CrsLibrary::setFavorite(const QString& definition, bool favorite)
{
    const QString trimmed = definition.trimmed();
    if (trimmed.isEmpty())
        return;
    if (favorite) {
        if (m_favorites.contains(trimmed))
            return;
        m_favorites.append(trimmed);
    } else if (!m_favorites.removeOne(trimmed)) {
        return;
    }
    m_dirty = true;
    markChanged();
}

const CrsEntry* CrsLibrary::find(const QString& id) const
{
    for (const CrsEntry& entry : m_entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

bool CrsLibrary::containsDefinition(const QString& definition) const
{
    const QString target = normalizedDefinition(definition);
    for (const CrsEntry& entry : m_entries) {
        if (normalizedDefinition(entry.definition) == target)
            return true;
    }
    return false;
}

bool CrsLibrary::add(const QString& name, const QString& definition, const QString& note,
                     CrsEntry* added, QString* error)
{
    return addWithOrigin(name, definition, note, QStringLiteral("custom"), added, error);
}

bool CrsLibrary::addWithOrigin(const QString& name, const QString& definition,
                               const QString& note, const QString& origin, CrsEntry* added,
                               QString* error)
{
    const QString trimmed = definition.trimmed();
    if (containsDefinition(trimmed)) {
        m_lastError = QStringLiteral("库里已经有同一条定义了");
        if (error)
            *error = m_lastError;
        return false;
    }
    CrsEntry entry;
    if (!buildEntry(name, trimmed, note, origin, &entry, error))
        return false;
    m_entries.append(entry);
    m_dirty = true;
    if (added)
        *added = entry;
    markChanged();
    return true;
}

// 只解析、不落库：编辑要「原地换掉那一条」，靠 addWithOrigin 的追加速语会变成两份。
bool CrsLibrary::buildEntry(const QString& name, const QString& definition, const QString& note,
                            const QString& origin, CrsEntry* out, QString* error)
{
    const auto fail = [this, error](const QString& message) {
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    };
    const QString trimmed = definition.trimmed();
    if (trimmed.isEmpty())
        return fail(QStringLiteral("参照系定义为空"));
    CrsBackend::CrsDefinition parsed;
    if (!m_backend.inspect(trimmed, &parsed))
        return fail(m_backend.lastError().isEmpty()
                        ? QStringLiteral("PROJ 无法识别这条参照系定义")
                        : m_backend.lastError());
    CrsEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.name = name.trimmed().isEmpty() ? (parsed.name.isEmpty() ? parsed.code : parsed.name)
                                         : name.trimmed();
    entry.definition = trimmed;
    entry.code = parsed.code;
    entry.projString = parsed.projString;
    entry.wkt = parsed.wkt;
    entry.note = note.trimmed();
    entry.origin = origin;
    entry.geographic = parsed.geographic;
    entry.projected = parsed.projected;
    *out = entry;
    return true;
}

bool CrsLibrary::update(const QString& id, const QString& name, const QString& definition,
                        const QString& note, QString* error)
{
    for (int index = 0; index < m_entries.size(); ++index) {
        if (m_entries[index].id != id)
            continue;
        const CrsEntry previous = m_entries.at(index);
        if (normalizedDefinition(previous.definition) == normalizedDefinition(definition) &&
            previous.name == name.trimmed() && previous.note == note.trimmed())
            return true;   // 没改动：不重解析也不置脏
        const QString trimmed = definition.trimmed();
        // 改成库里另一条已有的定义等于重复录入，编辑只许改成新的一条。
        if (normalizedDefinition(previous.definition) != normalizedDefinition(trimmed) &&
            containsDefinition(trimmed)) {
            m_lastError = QStringLiteral("库里已经有同一条定义了");
            if (error)
                *error = m_lastError;
            return false;
        }
        CrsEntry replacement;
        // 解析失败时不动库：编辑坏定义不该把原来那条也搭进去。
        if (!buildEntry(name, trimmed, note, previous.origin, &replacement, error))
            return false;
        replacement.id = previous.id;   // 保持标识不变：下拉与其它界面按 id 认它
        m_entries.replace(index, replacement);
        m_dirty = true;
        markChanged();
        return true;
    }
    if (error)
        *error = QStringLiteral("找不到要编辑的参照系条目");
    return false;
}

bool CrsLibrary::remove(const QString& id)
{
    for (int index = 0; index < m_entries.size(); ++index) {
        if (m_entries[index].id != id)
            continue;
        m_entries.removeAt(index);
        m_dirty = true;
        markChanged();
        return true;
    }
    return false;
}

int CrsLibrary::importFiles(const QStringList& paths, QStringList* failed, QString* error)
{
    int added = 0;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        const QString suffix = info.suffix().toLower();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (failed)
                failed->append(QStringLiteral("%1（打不开）").arg(info.fileName()));
            continue;
        }
        const QByteArray raw = file.readAll();
        file.close();
        if (suffix == QLatin1String("json")) {
            const QJsonDocument document = QJsonDocument::fromJson(raw);
            if (!document.isObject()) {
                if (failed)
                    failed->append(QStringLiteral("%1（不是 JSON 库）").arg(info.fileName()));
                continue;
            }
            const QJsonArray array = document.object().value(QStringLiteral("crs")).toArray();
            for (const QJsonValue& value : array) {
                const QJsonObject object = value.toObject();
                const QString definition = definitionOfObject(object);
                CrsEntry entry;
                QString addError;
                if (!addWithOrigin(object.value(QStringLiteral("name")).toString(), definition,
                                   object.value(QStringLiteral("note")).toString(),
                                   QStringLiteral("import"), &entry, &addError)) {
                    // 重复定义不算失败：整库来回导第二次不应该满屏报错。
                    if (!addError.contains(QStringLiteral("已经有"))) {
                        if (failed)
                            failed->append(QStringLiteral("%1（%2）").arg(info.fileName(), addError));
                    }
                    continue;
                }
                ++added;
            }
            continue;
        }
        if (suffix != QLatin1String("prj") && suffix != QLatin1String("wkt")) {
            if (failed)
                failed->append(QStringLiteral("%1（只支持 .prj/.wkt/.json）").arg(info.fileName()));
            continue;
        }
        const QString text = QString::fromUtf8(raw).trimmed();
        CrsEntry entry;
        QString addError;
        // .prj 里通常没有可读的名字，先用解析出的权威名，再退回文件名。
        if (!addWithOrigin(info.completeBaseName(), text,
                           QStringLiteral("导入自 %1").arg(info.fileName()),
                           QStringLiteral("import"), &entry, &addError)) {
            if (failed)
                failed->append(QStringLiteral("%1（%2）").arg(info.fileName(), addError));
            continue;
        }
        ++added;
    }
    if (added == 0 && error && failed && !failed->isEmpty())
        *error = QStringLiteral("没有可导入的定义：%1").arg(failed->join(QStringLiteral("；")));
    return added;
}

bool CrsLibrary::exportFile(const QString& path, const QStringList& ids, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    QList<CrsEntry> selected;
    for (const QString& id : ids) {
        const CrsEntry* entry = find(id);
        if (entry)
            selected.append(*entry);
    }
    if (selected.isEmpty())
        return fail(QStringLiteral("没有选中要导出的参照系"));
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (selected.size() == 1 && (suffix == QLatin1String("prj") || suffix == QLatin1String("wkt"))) {
        // 单一 .prj/.wkt 只放定义文本：QGIS/ARCGISPro 直接认这种文件。
        const CrsEntry& entry = selected.front();
        const QString text = !entry.wkt.isEmpty() ? entry.wkt
                                                 : (!entry.projString.isEmpty() ? entry.projString
                                                                                 : entry.definition);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(QStringLiteral("无法写入：%1（%2）").arg(path, file.errorString()));
        file.write(text.toUtf8());
        file.close();
        return true;
    }
    // 整库导出才带上收藏；只导选中几条时留空，免得导出的片段里出现没导的引用。
    const bool wholeLibrary = ids.size() >= m_entries.size();
    return writeJson(path, selected, wholeLibrary ? m_favorites : QStringList(), error);
}

void CrsLibrary::setChangeCallback(std::function<void()> callback)
{
    m_changed = std::move(callback);
}

void CrsLibrary::markChanged()
{
    if (m_changed)
        m_changed();
}

} // namespace View3DCrs

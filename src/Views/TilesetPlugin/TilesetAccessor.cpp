#include "TilesetAccessor.h"

#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/IAssetResponse.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <cstring>
#include <functional>
#include <span>

namespace View3DTiles {
namespace {

std::uint32_t readUint32(std::span<const std::byte> bytes, std::size_t offset)
{
    if (offset + 4u > bytes.size())
        return 0;
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

void appendUint32(std::vector<std::byte>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::byte>(value & 0xffu));
    bytes.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
    bytes.push_back(static_cast<std::byte>((value >> 16u) & 0xffu));
    bytes.push_back(static_cast<std::byte>((value >> 24u) & 0xffu));
}

std::vector<std::byte> readFile(const QString& path, bool* exists)
{
    QFile file(path);
    if (!file.exists()) {
        if (exists)
            *exists = false;
        return {};
    }
    if (exists)
        *exists = true;
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = file.readAll();
    std::vector<std::byte> result(static_cast<std::size_t>(bytes.size()));
    if (!bytes.isEmpty())
        std::memcpy(result.data(), bytes.constData(), result.size());
    return result;
}

class LocalResponse final : public CesiumAsync::IAssetResponse {
  public:
    LocalResponse(std::uint16_t status, std::string type, std::vector<std::byte> data)
        : m_status(status), m_type(std::move(type)), m_data(std::move(data))
    {
    }

    std::uint16_t statusCode() const override { return m_status; }
    std::string contentType() const override { return m_type; }
    const CesiumAsync::HttpHeaders& headers() const override { return m_headers; }
    std::span<const std::byte> data() const override { return m_data; }

  private:
    std::uint16_t m_status;
    std::string m_type;
    CesiumAsync::HttpHeaders m_headers;
    std::vector<std::byte> m_data;
};

class LocalRequest final : public CesiumAsync::IAssetRequest {
  public:
    LocalRequest(std::string url, std::unique_ptr<LocalResponse> response)
        : m_url(std::move(url)), m_response(std::move(response))
    {
    }

    const std::string& method() const override { return m_method; }
    const std::string& url() const override { return m_url; }
    const CesiumAsync::HttpHeaders& headers() const override { return m_headers; }
    const CesiumAsync::IAssetResponse* response() const override
    {
        return m_response.get();
    }

  private:
    std::string m_method = "GET";
    std::string m_url;
    CesiumAsync::HttpHeaders m_headers;
    std::unique_ptr<LocalResponse> m_response;
};

std::string contentTypeFor(const QString& key)
{
    const QString suffix = QFileInfo(key).suffix().toLower();
    if (suffix == QStringLiteral("json"))
        return "application/json";
    if (suffix == QStringLiteral("glb"))
        return "model/gltf-binary";
    if (suffix == QStringLiteral("gltf"))
        return "model/gltf+json";
    return "application/octet-stream";
}

} // namespace

TilesetAssetAccessor::TilesetAssetAccessor(const QString& rootDirectory)
    : m_root(QDir::cleanPath(QDir(rootDirectory).absolutePath()))
{
}

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> TilesetAssetAccessor::get(
    const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url,
    const std::vector<THeader>& headers)
{
    static_cast<void>(headers);
    // 文件读取与 glTF 外部资源解析都可能很重，统一放进 Cesium 的加载线程，
    // 避免初始 tileset.json 请求在 UI 线程同步读盘。
    const std::shared_ptr<TilesetAssetAccessor> self = shared_from_this();
    const std::string target = url;
    return asyncSystem.runInWorkerThread(
        [self, target]() -> std::shared_ptr<CesiumAsync::IAssetRequest> {
            try {
                return self->load(target);
            } catch (...) {
                return self->failureRequest(target);
            }
        });
}

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> TilesetAssetAccessor::request(
    const CesiumAsync::AsyncSystem& asyncSystem, const std::string& method,
    const std::string& url, const std::vector<THeader>& headers,
    const std::span<const std::byte>& contentPayload)
{
    static_cast<void>(method);
    static_cast<void>(contentPayload);
    return get(asyncSystem, url, headers);
}

void TilesetAssetAccessor::tick() noexcept
{
}

std::shared_ptr<CesiumAsync::IAssetRequest> TilesetAssetAccessor::load(const std::string& url)
{
    ++m_requests;
    bool withinRoot = false;
    const QString key = resolveRelative(
        QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size())), &withinRoot);
    if (!withinRoot || key.isEmpty()) {
        ++m_missing;
        warn(QStringLiteral("拒绝越界的瓦片资源 URL：%1")
                 .arg(QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size()))));
        return std::make_shared<LocalRequest>(
            url, std::make_unique<LocalResponse>(404, "text/plain", std::vector<std::byte> {}));
    }

    std::vector<std::byte> data;
    {
        std::scoped_lock lock(m_virtualMutex);
        const auto found = m_virtualContents.constFind(key);
        if (found != m_virtualContents.constEnd()) {
            data = buildComposite(found.value());
            if (data.empty()) {
                ++m_missing;
                warn(QStringLiteral("无法合成 3D Tiles 1.1 multiple contents：%1").arg(key));
                return std::make_shared<LocalRequest>(
                    url, std::make_unique<LocalResponse>(404, "text/plain",
                                                          std::vector<std::byte> {}));
            }
        }
    }

    if (data.empty()) {
        bool exists = false;
        const QString fullPath = QDir(m_root).filePath(key);
        data = readFile(fullPath, &exists);
        if (!exists) {
            ++m_missing;
            return std::make_shared<LocalRequest>(
                url, std::make_unique<LocalResponse>(404, "text/plain",
                                                      std::vector<std::byte> {}));
        }
        if (data.empty() && QFileInfo(fullPath).size() > 0) {
            warn(QStringLiteral("本地瓦片资源读取失败：%1").arg(key));
            return std::make_shared<LocalRequest>(
                url, std::make_unique<LocalResponse>(404, "text/plain",
                                                      std::vector<std::byte> {}));
        }
        if (key.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
            normalizeMultipleContents(key, &data);
    }

    return std::make_shared<LocalRequest>(
        url, std::make_unique<LocalResponse>(200, contentTypeFor(key), std::move(data)));
}

std::shared_ptr<CesiumAsync::IAssetRequest> TilesetAssetAccessor::failureRequest(
    const std::string& url)
{
    // 读盘/合成/JSON 解析任一失败都不把异常抛回 Cesium 的任务链；返回 500
    // 让加载器按普通请求失败处理，避免异常穿过 worker 顶层造成进程崩溃。
    try {
        return std::make_shared<LocalRequest>(
            url, std::make_unique<LocalResponse>(500, "text/plain",
                                                  std::vector<std::byte> {}));
    } catch (...) {
        return nullptr;
    }
}

bool TilesetAssetAccessor::normalizeMultipleContents(const QString& manifestKey,
                                                    std::vector<std::byte>* data)
{
    if (!data || data->empty())
        return false;
    const QByteArray bytes(reinterpret_cast<const char*>(data->data()),
                           static_cast<qsizetype>(data->size()));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return false;
    QJsonObject manifest = document.object();
    const QString version =
        manifest.value(QStringLiteral("asset")).toObject().value(QStringLiteral("version"))
            .toString();
    if (!version.startsWith(QStringLiteral("1.1")))
        return false;
    QJsonObject root = manifest.value(QStringLiteral("root")).toObject();
    if (root.isEmpty())
        return false;

    const QString parentDirRaw = QDir::cleanPath(QFileInfo(manifestKey).path());
    const QString parentDir =
        parentDirRaw == QStringLiteral(".") ? QString() : parentDirRaw;
    bool changed = false;
    bool failed = false;
    std::function<void(QJsonObject&)> visitTile = [&](QJsonObject& tile) {
        if (failed)
            return;
        const bool implicit = tile.value(QStringLiteral("implicitTiling")).isObject() ||
            tile.value(QStringLiteral("extensions")).toObject()
                .value(QStringLiteral("3DTILES_implicit_tiling"))
                .isObject();
        const QJsonArray contents = tile.value(QStringLiteral("contents")).toArray();
        if (!implicit && !contents.isEmpty()) {
            QStringList sources;
            sources.reserve(contents.size());
            bool valid = true;
            for (const QJsonValue& value : contents) {
                if (!value.isObject()) {
                    valid = false;
                    break;
                }
                const QJsonObject content = value.toObject();
                QString uri = content.value(QStringLiteral("uri")).toString();
                if (uri.isEmpty())
                    uri = content.value(QStringLiteral("url")).toString();
                if (uri.isEmpty() || uri.contains(QLatin1Char('{'))) {
                    valid = false;
                    break;
                }
                bool withinRoot = false;
                const QString joined = parentDir.isEmpty()
                    ? uri
                    : parentDir + QLatin1Char('/') + uri;
                const QString resolved = resolveRelative(joined, &withinRoot);
                if (!withinRoot || resolved.isEmpty()) {
                    valid = false;
                    break;
                }
                sources.append(resolved);
            }
            if (!valid) {
                failed = true;
                return;
            }
            if (sources.size() == 1) {
                tile.insert(QStringLiteral("content"), contents.at(0));
                tile.remove(QStringLiteral("contents"));
                changed = true;
            } else if (sources.size() > 1) {
                const QString virtualName = QStringLiteral("__view3d_multi_%1.cmpt")
                                                .arg(m_virtualSerial.fetch_add(1));
                // GLB 里的外部贴图 URI 相对内容所在目录解析。虚拟 cmpt 放在
                // 第一份内容旁边，才能让这些相对路径继续成立。
                QString sourceParent = QFileInfo(sources.front()).path();
                if (sourceParent == QStringLiteral("."))
                    sourceParent.clear();
                const QString virtualKey = QDir::cleanPath(
                    sourceParent.isEmpty() ? virtualName
                                           : sourceParent + QLatin1Char('/') + virtualName);
                {
                    std::scoped_lock lock(m_virtualMutex);
                    m_virtualContents.insert(virtualKey, sources);
                }
                const QString manifestDir = parentDir.isEmpty() ? QStringLiteral(".")
                                                                : parentDir;
                const QString contentUri = QDir::fromNativeSeparators(
                    QDir(manifestDir).relativeFilePath(virtualKey));
                QJsonObject content;
                content.insert(QStringLiteral("uri"),
                               contentUri.isEmpty() ? virtualKey : contentUri);
                tile.insert(QStringLiteral("content"), content);
                tile.remove(QStringLiteral("contents"));
                changed = true;
            }
        }
        QJsonArray children = tile.value(QStringLiteral("children")).toArray();
        for (qsizetype index = 0; index < children.size(); ++index) {
            if (!children.at(index).isObject())
                continue;
            QJsonObject child = children.at(index).toObject();
            visitTile(child);
            if (failed)
                return;
            children[index] = child;
        }
        if (!children.isEmpty())
            tile.insert(QStringLiteral("children"), children);
    };
    visitTile(root);
    if (failed) {
        warn(QStringLiteral("3D Tiles 1.1 multiple contents 含无法解析的资源：%1")
                 .arg(manifestKey));
        return false;
    }
    if (!changed)
        return false;
    manifest.insert(QStringLiteral("root"), root);
    QJsonDocument output(manifest);
    const QByteArray normalized = output.toJson(QJsonDocument::Compact);
    data->resize(static_cast<std::size_t>(normalized.size()));
    if (!normalized.isEmpty())
        std::memcpy(data->data(), normalized.constData(), data->size());
    return true;
}

std::vector<std::byte> TilesetAssetAccessor::buildComposite(const QStringList& sourceKeys)
{
    std::vector<std::byte> composite;
    composite.reserve(16u + 1024u);
    composite.push_back(std::byte { 'c' });
    composite.push_back(std::byte { 'm' });
    composite.push_back(std::byte { 'p' });
    composite.push_back(std::byte { 't' });
    appendUint32(composite, 1u);
    appendUint32(composite, 0u); // byteLength 稍后回填
    appendUint32(composite, static_cast<std::uint32_t>(sourceKeys.size()));

    for (const QString& sourceKey : sourceKeys) {
        bool exists = false;
        const std::vector<std::byte> source = readFile(QDir(m_root).filePath(sourceKey), &exists);
        if (!exists || source.size() < 12u ||
            readUint32(std::span<const std::byte>(source.data(), source.size()), 8u) !=
                source.size())
            return {};
        const std::uint32_t magic = readUint32(
            std::span<const std::byte>(source.data(), source.size()), 0u);
        constexpr std::uint32_t b3dmMagic = 0x6D643362u;
        constexpr std::uint32_t i3dmMagic = 0x6D643369u;
        constexpr std::uint32_t pntsMagic = 0x73746E70u;
        constexpr std::uint32_t cmptMagic = 0x74706D63u;
        constexpr std::uint32_t glbMagic = 0x46546C67u;
        if (magic == glbMagic) {
            // 3D Tiles 1.1 的 contents 允许直接是 GLB。cmpt 的内层头恰好与
            // GLB 头同形（magic/version/byteLength），Cesium 的 cmpt 转换器会
            // 再按内层 magic 分派到 glTF 读取器，因此可以直接嵌入。
            if (source.size() % 4u != 0u ||
                readUint32(std::span<const std::byte>(source.data(), source.size()), 4u) != 2u)
                return {};
        } else {
            if (source.size() % 8u != 0u)
                return {};
            if (magic != b3dmMagic && magic != i3dmMagic && magic != pntsMagic &&
                magic != cmptMagic)
                return {};
        }
        composite.insert(composite.end(), source.begin(), source.end());
    }
    if (composite.size() > 0xffffffffu)
        return {};
    const std::uint32_t byteLength = static_cast<std::uint32_t>(composite.size());
    composite[8] = static_cast<std::byte>(byteLength & 0xffu);
    composite[9] = static_cast<std::byte>((byteLength >> 8u) & 0xffu);
    composite[10] = static_cast<std::byte>((byteLength >> 16u) & 0xffu);
    composite[11] = static_cast<std::byte>((byteLength >> 24u) & 0xffu);
    ++m_multipleContents;
    return composite;
}

QString TilesetAssetAccessor::resolveRelative(const QString& url, bool* withinRoot) const
{
    if (withinRoot)
        *withinRoot = false;
    QString text = url;
    const qsizetype query = text.indexOf(QLatin1Char('?'));
    if (query >= 0)
        text.truncate(query);
    const qsizetype fragment = text.indexOf(QLatin1Char('#'));
    if (fragment >= 0)
        text.truncate(fragment);
    text = QUrl::fromPercentEncoding(text.toUtf8());
    text = QDir::fromNativeSeparators(text);
    if (text.isEmpty() || text.startsWith(QStringLiteral("//")) ||
        text.contains(QStringLiteral("://")) || QDir::isAbsolutePath(text))
        return {};
    const QString clean = QDir::cleanPath(text);
    if (clean == QStringLiteral("..") || clean.startsWith(QStringLiteral("../")))
        return {};
    const QString full = QDir::cleanPath(QDir(m_root).absoluteFilePath(clean));
    const QString root = QDir::cleanPath(m_root);
    if (full.compare(root, Qt::CaseInsensitive) != 0 &&
        !full.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive))
        return {};
    if (withinRoot)
        *withinRoot = true;
    return clean;
}

void TilesetAssetAccessor::warn(const QString& message) const
{
    std::scoped_lock lock(m_warningMutex);
    if (m_warnings.size() >= 200 || m_warnings.contains(message))
        return;
    m_warnings.append(message);
}

QStringList TilesetAssetAccessor::warnings() const
{
    std::scoped_lock lock(m_warningMutex);
    return m_warnings;
}

} // namespace View3DTiles

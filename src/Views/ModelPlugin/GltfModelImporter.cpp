#include "GltfModelImporter.h"


#include <CesiumGltf/Model.h>
#include <CesiumGltf/AccessorUtility.h>
#include <CesiumGltf/AccessorView.h>
#include <CesiumGltfReader/GltfReader.h>
#include <CesiumGltfContent/GltfUtilities.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/IAssetAccessor.h>
#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/IAssetResponse.h>
#include <CesiumAsync/ITaskProcessor.h>

#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QSet>

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <span>
#include <vector>
#include <variant>
#include <thread>
#include <functional>
#include <limits>
#include <optional>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <cmath>

namespace View3DModel {

namespace {

class LocalTaskProcessor final : public CesiumAsync::ITaskProcessor {
public:
    void startTask(std::function<void()> task) override
    {
        std::thread(std::move(task)).detach();
    }
};

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
    const CesiumAsync::IAssetResponse* response() const override { return m_response.get(); }

  private:
    std::string m_method {"GET"};
    std::string m_url;
    std::unique_ptr<LocalResponse> m_response;
    CesiumAsync::HttpHeaders m_headers;
};

// 只服务本地文件的资源访问器：.gltf 的外部 buffer/贴图已由 prepareExternalGltf
// 改写成 file:/// 绝对路径，所以不需要 curl，也不会碰到网络。URL 解析失败或文件
// 缺失一律返回 404，让 Cesium 的读取器按普通缺资源处理，不把异常抛进任务链。
class LocalFileAssetAccessor final : public CesiumAsync::IAssetAccessor {
  public:
    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> get(
        const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url,
        const std::vector<THeader>& headers) override
    {
        static_cast<void>(headers);
        const std::string target = url;
        return asyncSystem.runInWorkerThread(
            [target]() -> std::shared_ptr<CesiumAsync::IAssetRequest> {
                return load(target);
            });
    }

    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> request(
        const CesiumAsync::AsyncSystem& asyncSystem, const std::string& method,
        const std::string& url, const std::vector<THeader>& headers,
        const std::span<const std::byte>& contentPayload) override
    {
        static_cast<void>(method);
        static_cast<void>(contentPayload);
        return get(asyncSystem, url, headers);
    }

    void tick() noexcept override {}

  private:
    static std::shared_ptr<CesiumAsync::IAssetRequest> load(const std::string& url)
    {
        const QUrl parsed {QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size()))};
        if (parsed.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) != 0 ||
            !parsed.isLocalFile()) {
            return std::make_shared<LocalRequest>(
                url, std::make_unique<LocalResponse>(404, "text/plain", std::vector<std::byte> {}));
        }
        QFile file(parsed.toLocalFile());
        if (!file.exists()) {
            return std::make_shared<LocalRequest>(
                url, std::make_unique<LocalResponse>(404, "text/plain",
                                                    std::vector<std::byte> {}));
        }
        if (!file.open(QIODevice::ReadOnly)) {
            return std::make_shared<LocalRequest>(
                url, std::make_unique<LocalResponse>(500, "text/plain",
                                                    std::vector<std::byte> {}));
        }
        const QByteArray bytes = file.readAll();
        std::vector<std::byte> data(static_cast<std::size_t>(bytes.size()));
        if (!bytes.isEmpty())
            std::memcpy(data.data(), bytes.constData(), data.size());
        const QString suffix = QFileInfo(file).suffix().toLower();
        const std::string type = suffix == QStringLiteral("bin")
            ? "application/octet-stream"
            : (suffix == QStringLiteral("json") ? "application/json" : "image/*");
        return std::make_shared<LocalRequest>(
            url, std::make_unique<LocalResponse>(200, type, std::move(data)));
    }
};

struct PreparedExternalGltf {
    bool success = false;
    QByteArray json;
    QString errorCode;
    QString errorMessage;
    int textureFallbackCount = 0;
};

double finiteClamped01(double value, double fallback, const QString& field,
                       std::int32_t materialIndex, QStringList* warnings)
{
    if (!std::isfinite(value)) {
        if (warnings) warnings->append(QStringLiteral(
            "材质 %1 的 %2 不是有限值，已使用默认值 %3")
            .arg(materialIndex).arg(field).arg(fallback));
        return fallback;
    }
    const double clamped = qBound(0.0, value, 1.0);
    if (clamped != value && warnings) {
        warnings->append(QStringLiteral(
            "材质 %1 的 %2=%3 超出 0～1，已钳制为 %4")
            .arg(materialIndex).arg(field).arg(value).arg(clamped));
    }
    return clamped;
}

double finiteNonNegative(double value, double fallback, const QString& field,
                         std::int32_t materialIndex, QStringList* warnings)
{
    if (!std::isfinite(value) || value < 0.0) {
        if (warnings) warnings->append(QStringLiteral(
            "材质 %1 的 %2=%3 无效，已使用默认值 %4")
            .arg(materialIndex).arg(field).arg(value).arg(fallback));
        return fallback;
    }
    return value;
}

std::int32_t supportedTexCoord(std::int32_t value, const QString& field,
                               std::int32_t materialIndex, QStringList* warnings)
{
    if (value == 0 || value == 1) return value;
    if (warnings) warnings->append(QStringLiteral(
        "材质 %1 的 %2 使用不支持的 TEXCOORD_%3，已禁用对应纹理")
        .arg(materialIndex).arg(field).arg(value));
    return -1;
}

bool vertexUv(const MeshVertexDto& vertex, std::int32_t set, glm::dvec2* uv)
{
    if (!uv) return false;
    if (set == 0 && vertex.hasTexCoord) {
        *uv = glm::dvec2(vertex.u, vertex.v);
        return std::isfinite(vertex.u) && std::isfinite(vertex.v);
    }
    if (set == 1 && vertex.hasTexCoord1) {
        *uv = glm::dvec2(vertex.u1, vertex.v1);
        return std::isfinite(vertex.u1) && std::isfinite(vertex.v1);
    }
    return false;
}

bool preparePrimitiveTangents(ModelMeshDto* mesh, ModelPrimitiveDto* primitive,
                              std::int32_t texCoordSet, QString* reason)
{
    if (reason) reason->clear();
    if (!mesh || !primitive || primitive->topology != ModelPrimitiveTopology::Triangles ||
        primitive->firstVertex > mesh->vertices.size() ||
        primitive->vertexCount > mesh->vertices.size() - primitive->firstVertex ||
        primitive->firstIndex > mesh->indices.size() ||
        primitive->indexCount > mesh->indices.size() - primitive->firstIndex ||
        primitive->indexCount % 3u != 0u) {
        if (reason) *reason = QStringLiteral("primitive 范围或拓扑无效");
        return false;
    }
    bool explicitTangents = true;
    for (std::uint32_t offset = 0; offset < primitive->vertexCount; ++offset) {
        const MeshVertexDto& vertex = mesh->vertices[primitive->firstVertex + offset];
        glm::dvec2 uv;
        if (!vertex.hasNormal || !vertexUv(vertex, texCoordSet, &uv)) {
            if (reason) *reason = QStringLiteral("缺少法线或指定 UV 集");
            return false;
        }
        explicitTangents = explicitTangents && vertex.hasTangent;
    }

    std::vector<glm::dvec3> tangents(primitive->vertexCount, glm::dvec3(0.0));
    std::vector<glm::dvec3> bitangents(primitive->vertexCount, glm::dvec3(0.0));
    std::vector<double> handedness(primitive->vertexCount, 1.0);
    if (explicitTangents) {
        for (std::uint32_t offset = 0; offset < primitive->vertexCount; ++offset) {
            const MeshVertexDto& vertex = mesh->vertices[primitive->firstVertex + offset];
            tangents[offset] = glm::dvec3(vertex.tx, vertex.ty, vertex.tz);
            handedness[offset] = vertex.tw;
        }
    } else {
        for (std::uint32_t offset = 0; offset < primitive->indexCount; offset += 3u) {
            const std::uint32_t global0 = mesh->indices[primitive->firstIndex + offset];
            const std::uint32_t global1 = mesh->indices[primitive->firstIndex + offset + 1u];
            const std::uint32_t global2 = mesh->indices[primitive->firstIndex + offset + 2u];
            if (global0 < primitive->firstVertex || global1 < primitive->firstVertex ||
                global2 < primitive->firstVertex ||
                global0 >= primitive->firstVertex + primitive->vertexCount ||
                global1 >= primitive->firstVertex + primitive->vertexCount ||
                global2 >= primitive->firstVertex + primitive->vertexCount) {
                if (reason) *reason = QStringLiteral("索引超出 primitive 顶点范围");
                return false;
            }
            const std::uint32_t local0 = global0 - primitive->firstVertex;
            const std::uint32_t local1 = global1 - primitive->firstVertex;
            const std::uint32_t local2 = global2 - primitive->firstVertex;
            const MeshVertexDto& v0 = mesh->vertices[global0];
            const MeshVertexDto& v1 = mesh->vertices[global1];
            const MeshVertexDto& v2 = mesh->vertices[global2];
            const glm::dvec3 p0(v0.x, v0.y, v0.z);
            const glm::dvec3 p1(v1.x, v1.y, v1.z);
            const glm::dvec3 p2(v2.x, v2.y, v2.z);
            glm::dvec2 uv0, uv1, uv2;
            if (!vertexUv(v0, texCoordSet, &uv0) || !vertexUv(v1, texCoordSet, &uv1) ||
                !vertexUv(v2, texCoordSet, &uv2)) {
                if (reason) *reason = QStringLiteral("指定 UV 集不完整");
                return false;
            }
            const glm::dvec3 edge1 = p1 - p0;
            const glm::dvec3 edge2 = p2 - p0;
            const glm::dvec2 delta1 = uv1 - uv0;
            const glm::dvec2 delta2 = uv2 - uv0;
            const double determinant = delta1.x * delta2.y - delta1.y * delta2.x;
            if (!std::isfinite(determinant) || qAbs(determinant) <= 1e-12) continue;
            const double inverse = 1.0 / determinant;
            const glm::dvec3 tangent = (edge1 * delta2.y - edge2 * delta1.y) * inverse;
            const glm::dvec3 bitangent = (edge2 * delta1.x - edge1 * delta2.x) * inverse;
            for (const std::uint32_t local : {local0, local1, local2}) {
                tangents[local] += tangent;
                bitangents[local] += bitangent;
            }
        }
    }

    for (std::uint32_t offset = 0; offset < primitive->vertexCount; ++offset) {
        MeshVertexDto& vertex = mesh->vertices[primitive->firstVertex + offset];
        const glm::dvec3 normal(vertex.nx, vertex.ny, vertex.nz);
        glm::dvec3 tangent = tangents[offset] - normal * glm::dot(normal, tangents[offset]);
        const double tangentLength = glm::length(tangent);
        if (!std::isfinite(tangentLength) || tangentLength <= 1e-12) {
            if (reason) *reason = QStringLiteral("UV 退化或无法形成有限切线");
            return false;
        }
        tangent /= tangentLength;
        double sign = handedness[offset];
        if (!explicitTangents) {
            const double orientation = glm::dot(glm::cross(normal, tangent), bitangents[offset]);
            sign = orientation < 0.0 ? -1.0 : 1.0;
        }
        if (!std::isfinite(sign) || sign <= 0.0) {
            if (reason) *reason = QStringLiteral(
                "PBR_NORMAL_TANGENT_HANDEDNESS_UNSUPPORTED");
            return false;
        }
        vertex.tx = tangent.x;
        vertex.ty = tangent.y;
        vertex.tz = tangent.z;
        vertex.tw = 1.0;
        vertex.hasTangent = true;
    }
    primitive->tangentSource = explicitTangents
        ? ModelTangentSource::Explicit : ModelTangentSource::Generated;
    primitive->normalMappingEnabled = true;
    return true;
}

GltfExtensionDeclarations parseExtensionDeclarations(const QByteArray& json)
{
    GltfExtensionDeclarations declarations;
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) return declarations;
    declarations.recognized = true;
    const QJsonObject root = document.object();
    for (const QJsonValue& value : root.value(QStringLiteral("extensionsUsed")).toArray()) {
        if (value.isString()) declarations.used.append(value.toString());
    }
    for (const QJsonValue& value : root.value(QStringLiteral("extensionsRequired")).toArray()) {
        if (value.isString()) declarations.required.append(value.toString());
    }
    return declarations;
}

GltfExtensionDeclarations parseGlbExtensionDeclarations(
    std::span<const std::byte> bytes)
{
    auto readUint32 = [&](std::size_t offset) -> std::uint32_t {
        if (offset + 4u > bytes.size()) return 0;
        return static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
            (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
            (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
    };
    constexpr std::uint32_t glbMagic = 0x46546C67u;
    constexpr std::uint32_t jsonChunkType = 0x4E4F534Au;
    if (bytes.size() < 20u || readUint32(0) != glbMagic ||
        readUint32(16) != jsonChunkType) return {};
    const std::uint32_t jsonLength = readUint32(12);
    if (jsonLength > bytes.size() - 20u) return {};
    return parseExtensionDeclarations(QByteArray(
        reinterpret_cast<const char*>(bytes.data() + 20u),
        static_cast<qsizetype>(jsonLength)));
}

const QSet<QString>& supportedGltfExtensions()
{
    static const QSet<QString> extensions {
        QStringLiteral("KHR_draco_mesh_compression"),
        QStringLiteral("EXT_meshopt_compression"),
        QStringLiteral("KHR_mesh_quantization"),
        QStringLiteral("KHR_texture_basisu"),
        QStringLiteral("EXT_texture_webp")
    };
    return extensions;
}

bool isPathWithin(const QString& rootPath, const QString& filePath)
{
    const QString root = QDir::cleanPath(QFileInfo(rootPath).canonicalFilePath());
    const QString file = QDir::cleanPath(QFileInfo(filePath).canonicalFilePath());
    if (root.isEmpty() || file.isEmpty()) return false;
    if (file.compare(root, Qt::CaseInsensitive) == 0) return true;
    return file.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
}

PreparedExternalGltf prepareExternalGltf(
    const QByteArray& jsonBytes, const QFileInfo& modelFile,
    const QStringList& textureSearchPaths)
{
    PreparedExternalGltf prepared;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (!document.isObject()) {
        prepared.errorCode = QStringLiteral("GLTF_JSON_INVALID");
        prepared.errorMessage = QStringLiteral("glTF JSON 无效：%1").arg(parseError.errorString());
        return prepared;
    }
    QJsonObject root = document.object();
    const QString modelDirectory = modelFile.absolutePath();
    for (const QString& collection : {QStringLiteral("buffers"), QStringLiteral("images")}) {
        QJsonArray items = root.value(collection).toArray();
        for (qsizetype index = 0; index < items.size(); ++index) {
            QJsonObject item = items[index].toObject();
            const QString uri = QUrl::fromPercentEncoding(
                item.value(QStringLiteral("uri")).toString().toUtf8());
            if (uri.isEmpty() || uri.startsWith(QStringLiteral("data:"))) continue;
            const QUrl parsed(uri);
            if (!parsed.scheme().isEmpty() || QDir::isAbsolutePath(uri) ||
                uri == QStringLiteral("..") || uri.startsWith(QStringLiteral("../")) ||
                uri.contains(QStringLiteral("/../")) || uri.startsWith(QStringLiteral("..\\")) ||
                uri.contains(QStringLiteral("\\..\\"))) {
                prepared.errorCode = QStringLiteral("GLTF_EXTERNAL_URI_REJECTED");
                prepared.errorMessage = QStringLiteral("外部资源路径不安全或超出允许目录：%1")
                                            .arg(uri);
                return prepared;
            }
            QString resolved = QDir(modelDirectory).filePath(QDir::fromNativeSeparators(uri));
            if (!QFileInfo(resolved).isFile() && collection == QStringLiteral("images")) {
                for (const QString& searchPath : textureSearchPaths) {
                    const QString exact = QDir(searchPath).filePath(
                        QDir::fromNativeSeparators(uri));
                    const QString byName = QDir(searchPath).filePath(QFileInfo(uri).fileName());
                    if (QFileInfo(exact).isFile() && isPathWithin(searchPath, exact)) {
                        resolved = exact;
                        break;
                    }
                    if (QFileInfo(byName).isFile() && isPathWithin(searchPath, byName)) {
                        resolved = byName;
                        break;
                    }
                }
                if (QFileInfo(resolved).isFile()) ++prepared.textureFallbackCount;
            }
            bool allowedPath = isPathWithin(modelDirectory, resolved);
            if (collection == QStringLiteral("images") && !allowedPath) {
                for (const QString& searchPath : textureSearchPaths)
                    allowedPath = allowedPath || isPathWithin(searchPath, resolved);
            }
            if (!QFileInfo(resolved).isFile() || !allowedPath) {
                prepared.errorCode = collection == QStringLiteral("images")
                    ? QStringLiteral("GLTF_TEXTURE_MISSING")
                    : QStringLiteral("GLTF_BUFFER_MISSING");
                prepared.errorMessage = QStringLiteral(
                    "找不到 glTF 外部资源：%1；模型目录：%2；纹理搜索路径：%3")
                                            .arg(uri, modelDirectory,
                                                 textureSearchPaths.join(QLatin1Char(';')));
                return prepared;
            }
            item.insert(QStringLiteral("uri"),
                        QUrl::fromLocalFile(QFileInfo(resolved).canonicalFilePath())
                            .toString(QUrl::FullyEncoded));
            items[index] = item;
        }
        root.insert(collection, items);
    }
    prepared.success = true;
    prepared.json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return prepared;
}

// Auto 上轴的内容判据：SketchUp / Babylon 这类导出器常把 Z-up 原样写进 glTF，
// asset 里没有任何上轴声明，按格式原生 Y-up 读进来整份成果就"立"起来——
// 实测 测试三维模型0815：x 3.7 km、y 3.9 km、z 0.58~237 m，高度显然在 Z；
// 立起来后果是用户只能把相机转到近乎正俯视才看得平，正好撞上导航的俯仰限位。
// 只在「Z 是唯一最薄轴、明显薄于另外两轴，且达到场地级尺度」时改判 Z-up；
// 正常 Y-up 成果（楼、车、墙、杆）的最薄轴通常就是 Y，不会命中。
ModelSourceUpAxis detectModelUpAxis(const CesiumGltf::Model& model)
{
    if (model.meshes.empty() || model.nodes.empty())
        return ModelSourceUpAxis::YUp;
    std::vector<int32_t> roots;
    const int32_t sceneIndex = model.scene >= 0 ? model.scene : 0;
    if (sceneIndex >= 0 && static_cast<std::size_t>(sceneIndex) < model.scenes.size())
        roots = model.scenes[static_cast<std::size_t>(sceneIndex)].nodes;
    if (roots.empty()) {
        std::vector<bool> isChild(model.nodes.size(), false);
        for (const CesiumGltf::Node& node : model.nodes) {
            for (const int32_t child : node.children) {
                if (child >= 0 && static_cast<std::size_t>(child) < isChild.size())
                    isChild[static_cast<std::size_t>(child)] = true;
            }
        }
        for (std::size_t index = 0; index < isChild.size(); ++index) {
            if (!isChild[index])
                roots.push_back(static_cast<int32_t>(index));
        }
    }
    glm::dvec3 minimum(std::numeric_limits<double>::max());
    glm::dvec3 maximum(std::numeric_limits<double>::lowest());
    bool any = false;
    std::vector<bool> active(model.nodes.size(), false);
    std::vector<std::pair<int32_t, glm::dmat4>> stack;
    stack.reserve(model.nodes.size());
    for (const int32_t root : roots)
        stack.emplace_back(root, glm::dmat4(1.0));
    while (!stack.empty()) {
        const auto [nodeIndex, parentWorld] = stack.back();
        stack.pop_back();
        if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= model.nodes.size())
            continue;
        const std::size_t index = static_cast<std::size_t>(nodeIndex);
        if (active[index])
            continue;
        const CesiumGltf::Node& node = model.nodes[index];
        const std::optional<glm::dmat4> local =
            CesiumGltfContent::GltfUtilities::getNodeTransform(node);
        if (!local)
            continue;
        const glm::dmat4 world = parentWorld * *local;
        if (node.mesh >= 0 && static_cast<std::size_t>(node.mesh) < model.meshes.size()) {
            for (const CesiumGltf::MeshPrimitive& primitive :
                 model.meshes[static_cast<std::size_t>(node.mesh)].primitives) {
                const auto position = primitive.attributes.find("POSITION");
                if (position == primitive.attributes.end() || position->second < 0 ||
                    static_cast<std::size_t>(position->second) >= model.accessors.size())
                    continue;
                const CesiumGltf::Accessor& accessor =
                    model.accessors[static_cast<std::size_t>(position->second)];
                if (accessor.min.size() < 3 || accessor.max.size() < 3)
                    continue;
                for (int corner = 0; corner < 8; ++corner) {
                    const glm::dvec4 cornerPoint(
                        (corner & 1) ? accessor.max[0] : accessor.min[0],
                        (corner & 2) ? accessor.max[1] : accessor.min[1],
                        (corner & 4) ? accessor.max[2] : accessor.min[2], 1.0);
                    const glm::dvec4 point = world * cornerPoint;
                    minimum = glm::min(minimum, glm::dvec3(point));
                    maximum = glm::max(maximum, glm::dvec3(point));
                    any = true;
                }
            }
        }
        active[index] = true;
        for (const int32_t child : node.children)
            stack.emplace_back(child, world);
        active[index] = false;
    }
    if (!any)
        return ModelSourceUpAxis::YUp;
    const glm::dvec3 span = maximum - minimum;
    // 保守判据：Z 是唯一最薄轴（不到 X 的一半、也不到 Y 的 35%），且整体达到
    // 场地级尺度（≥500 m）。正常 Y-up 成果（楼、车、墙、杆）要么最薄轴是 Y，
    // 要么尺度不够，都不会命中；不够判的可以在导入选项里显式选轴向。
    const double maxSpan = std::max({span.x, span.y, span.z});
    const bool flip = span.z > 0.0 && span.z < span.x * 0.5 && span.z < span.y * 0.35 &&
        maxSpan >= 500.0;
    std::cerr << "VIEW3D_GLTF_UP_AXIS span=" << span.x << "," << span.y << "," << span.z
              << " max=" << maxSpan << " roots=" << roots.size()
              << " flip=" << (flip ? 1 : 0) << std::endl;
    return flip ? ModelSourceUpAxis::ZUp : ModelSourceUpAxis::YUp;
}

} // namespace

GltfExtensionDeclarations GltfModelImporter::inspectExtensionDeclarations(
    const std::byte* data, std::size_t size)
{
    const std::span<const std::byte> content(data, size);
    const GltfExtensionDeclarations glb = parseGlbExtensionDeclarations(content);
    if (glb.recognized) return glb;
    return parseExtensionDeclarations(QByteArray(
        reinterpret_cast<const char*>(content.data()),
        static_cast<qsizetype>(content.size())));
}

bool GltfModelImporter::supportsExtension(const QString& extension)
{
    return supportedGltfExtensions().contains(extension);
}

ModelReadResult GltfModelImporter::importFile(
    const QString& path, const ModelImportOptions& options) const
{
    ModelReadResult output;
    if (!std::isfinite(options.unitScale) || options.unitScale <= 0.0 ||
        !std::isfinite(options.localOrigin[0]) ||
        !std::isfinite(options.localOrigin[1]) ||
        !std::isfinite(options.localOrigin[2]) || options.rayIndexLeafSize < 2 ||
        options.rayIndexLeafSize > 64) {
        output.error.code = QStringLiteral("MODEL_IMPORT_OPTIONS_INVALID");
        output.error.message = QStringLiteral("模型单位比例、局部原点或 BVH 叶大小无效");
        return output;
    }
    const QFileInfo fileInfo(path);
    std::ifstream input(fileInfo.absoluteFilePath().toStdWString(), std::ios::binary | std::ios::ate);
    if (!input) {
        output.error.code = QStringLiteral("MODEL_FILE_OPEN_FAILED");
        output.error.message = QStringLiteral("无法打开模型文件");
        output.error.source = path;
        return output;
    }
    const std::streamsize size = input.tellg();
    input.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        output.error.code = QStringLiteral("MODEL_FILE_READ_FAILED");
        output.error.message = QStringLiteral("模型文件读取不完整");
        return output;
    }

    CesiumGltfReader::GltfReader reader;
    CesiumGltfReader::GltfReaderResult result;
    GltfExtensionDeclarations extensionDeclarations;
    int textureFallbackCount = 0;
    if (fileInfo.suffix().compare(QStringLiteral("gltf"), Qt::CaseInsensitive) == 0) {
        const QByteArray jsonBytes(reinterpret_cast<const char*>(bytes.data()),
                                   static_cast<qsizetype>(bytes.size()));
        extensionDeclarations = inspectExtensionDeclarations(bytes.data(), bytes.size());
        const PreparedExternalGltf prepared = prepareExternalGltf(
            jsonBytes, fileInfo, options.textureSearchPaths);
        if (!prepared.success) {
            output.error.code = prepared.errorCode;
            output.error.message = prepared.errorMessage;
            return output;
        }
        textureFallbackCount = prepared.textureFallbackCount;
        QTemporaryDir patchedDirectory;
        if (!patchedDirectory.isValid()) {
            output.error.code = QStringLiteral("GLTF_TEMPORARY_FILE_FAILED");
            output.error.message = QStringLiteral("无法创建 glTF 临时解析目录");
            return output;
        }
        const QString patchedPath = patchedDirectory.filePath(QStringLiteral("model.gltf"));
        QFile patchedFile(patchedPath);
        if (!patchedFile.open(QIODevice::WriteOnly) ||
            patchedFile.write(prepared.json) != prepared.json.size()) {
            output.error.code = QStringLiteral("GLTF_TEMPORARY_FILE_FAILED");
            output.error.message = QStringLiteral("无法写入 glTF 临时解析文件");
            return output;
        }
        patchedFile.close();
        CesiumAsync::AsyncSystem asyncSystem(
            std::make_shared<LocalTaskProcessor>());
        auto accessor = std::make_shared<LocalFileAssetAccessor>();
        const std::string url =
            QUrl::fromLocalFile(patchedPath).toString(QUrl::FullyEncoded).toStdString();
        result = reader.loadGltf(asyncSystem, url, {}, accessor).waitInMainThread();
    } else {
        extensionDeclarations = inspectExtensionDeclarations(bytes.data(), bytes.size());
        result = reader.readGltf(std::span<const std::byte>(bytes.data(), bytes.size()));
    }
    if (!result.model || !result.errors.empty()) {
        output.error.code = QStringLiteral("GLTF_PARSE_FAILED");
        output.error.message = result.errors.empty()
            ? QStringLiteral("Cesium Native 无法解析 GLB/glTF")
            : QStringLiteral("Cesium Native 无法解析 GLB/glTF：%1")
                  .arg(QString::fromStdString(result.errors.front()));
        return output;
    }

    for (const std::string& warning : result.warnings)
        output.warnings.append(QStringLiteral("Cesium Native：%1")
                                   .arg(QString::fromStdString(warning)));
    const QSet<QString>& supportedExtensions = supportedGltfExtensions();
    QSet<QString> requiredExtensions;
    for (const QString& extension : extensionDeclarations.required) {
        requiredExtensions.insert(extension);
        if (!supportedExtensions.contains(extension)) {
            output.error.code = QStringLiteral("GLTF_REQUIRED_EXTENSION_UNSUPPORTED");
            output.error.message = QStringLiteral("glTF 声明了系统不支持的必需扩展：%1")
                                       .arg(extension);
            output.error.source = path;
            return output;
        }
    }
    int unsupportedUsedExtensionCount = 0;
    for (const QString& extension : extensionDeclarations.used) {
        if (!supportedExtensions.contains(extension) &&
            !requiredExtensions.contains(extension)) {
            ++unsupportedUsedExtensionCount;
            output.warnings.append(
                QStringLiteral("glTF 使用了未识别的非必需扩展，已忽略：%1")
                    .arg(extension));
        }
    }

    std::uint64_t primitiveCount = 0;
    std::uint64_t vertexCount = 0;
    std::uint64_t normalCount = 0;
    std::uint64_t texCoordCount = 0;
    auto meshDto = std::make_shared<ModelMeshDto>();
    ModelSourceUpAxis upAxis = options.sourceUpAxis;
    if (upAxis == ModelSourceUpAxis::Auto) {
        upAxis = detectModelUpAxis(*result.model);
        if (upAxis == ModelSourceUpAxis::ZUp) {
            output.warnings.append(QStringLiteral(
                "按包围盒判定为 Z 向上（glTF 默认按 Y 向上读会让整份成果立起来），"
                "已按 Z-up 导入；若判错可在导入选项里显式选择轴向"));
        }
    }
    glm::dmat4 axisTransform(1.0);
    if (upAxis == ModelSourceUpAxis::ZUp) {
        axisTransform[1] = glm::dvec4(0.0, 0.0, -1.0, 0.0);
        axisTransform[2] = glm::dvec4(0.0, 1.0, 0.0, 0.0);
    }
    const glm::dmat4 importTransform = glm::translate(
        glm::dmat4(1.0),
        glm::dvec3(-options.localOrigin[0], -options.localOrigin[1],
                   -options.localOrigin[2])) *
        axisTransform * glm::scale(glm::dmat4(1.0), glm::dvec3(options.unitScale));
    meshDto->textures.resize(result.model->textures.size());
    for (std::size_t textureIndex = 0; textureIndex < result.model->textures.size(); ++textureIndex) {
        const auto& texture = result.model->textures[textureIndex];
        if (texture.source < 0 ||
            static_cast<std::size_t>(texture.source) >= result.model->images.size()) {
            continue;
        }
        const auto& image = result.model->images[texture.source];
        if (!image.pAsset || image.pAsset->width <= 0 || image.pAsset->height <= 0 ||
            image.pAsset->bytesPerChannel != 1 || image.pAsset->pixelData.empty()) {
            continue;
        }
        ModelTextureDto& textureDto = meshDto->textures[textureIndex];
        if (texture.sampler >= 0 &&
            static_cast<std::size_t>(texture.sampler) < result.model->samplers.size()) {
            const auto& sampler = result.model->samplers[texture.sampler];
            textureDto.magFilter = sampler.magFilter.value_or(9729);
            textureDto.minFilter = sampler.minFilter.value_or(9987);
            textureDto.wrapS = sampler.wrapS;
            textureDto.wrapT = sampler.wrapT;
        }
        textureDto.width = image.pAsset->width;
        textureDto.height = image.pAsset->height;
        const std::size_t pixelCount =
            static_cast<std::size_t>(textureDto.width) * static_cast<std::size_t>(textureDto.height);
        textureDto.rgba8.resize(pixelCount * 4u);
        const std::size_t channels = static_cast<std::size_t>(image.pAsset->channels);
        if (channels < 1u || channels > 4u || image.pAsset->pixelData.size() < pixelCount * channels) {
            textureDto = {};
            continue;
        }
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const std::byte* source = image.pAsset->pixelData.data() + pixel * channels;
            std::byte* target = textureDto.rgba8.data() + pixel * 4u;
            if (channels == 1u || channels == 2u) {
                target[0] = source[0];
                target[1] = source[0];
                target[2] = source[0];
                target[3] = channels == 2u ? source[1] : std::byte{255};
            } else {
                target[0] = source[0];
                target[1] = source[1];
                target[2] = source[2];
                target[3] = channels == 4u ? source[3] : std::byte{255};
            }
        }
    }
    struct MeshInstance {
        std::int32_t meshIndex;
        std::uint32_t nodeDtoIndex;
    };
    std::vector<MeshInstance> meshInstances;
    std::vector<bool> activeNodes(result.model->nodes.size(), false);
    bool hierarchyValid = true;
    std::function<void(std::int32_t, const glm::dmat4&, std::int32_t)> visitNode;
    visitNode = [&](std::int32_t nodeIndex, const glm::dmat4& parentTransform,
                    std::int32_t parentDtoIndex) {
        if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= result.model->nodes.size() ||
            activeNodes[nodeIndex]) {
            hierarchyValid = false;
            return;
        }
        activeNodes[nodeIndex] = true;
        const CesiumGltf::Node& node = result.model->nodes[nodeIndex];
        const auto localTransform = CesiumGltfContent::GltfUtilities::getNodeTransform(node);
        if (!localTransform) {
            hierarchyValid = false;
            activeNodes[nodeIndex] = false;
            return;
        }
        const glm::dmat4 worldTransform = parentTransform * *localTransform;
        ModelNodeDto nodeDto;
        nodeDto.name = node.name.empty()
                           ? QStringLiteral("节点 %1").arg(nodeIndex)
                           : QString::fromStdString(node.name);
        nodeDto.sourceNodeIndex = nodeIndex;
        nodeDto.meshIndex = node.mesh;
        nodeDto.parentIndex = parentDtoIndex;
        for (glm::length_t column = 0; column < 4; ++column) {
            for (glm::length_t row = 0; row < 4; ++row) {
                const std::size_t offset = static_cast<std::size_t>(column * 4 + row);
                nodeDto.localTransform[offset] = parentDtoIndex < 0
                    ? worldTransform[column][row] : (*localTransform)[column][row];
                nodeDto.worldTransform[offset] = worldTransform[column][row];
            }
        }
        const std::uint32_t nodeDtoIndex = static_cast<std::uint32_t>(meshDto->nodes.size());
        meshDto->nodes.push_back(std::move(nodeDto));
        if (parentDtoIndex >= 0) {
            meshDto->nodes[static_cast<std::size_t>(parentDtoIndex)].children.push_back(nodeDtoIndex);
        } else {
            meshDto->rootNodes.push_back(nodeDtoIndex);
        }
        if (node.mesh >= 0) {
            if (static_cast<std::size_t>(node.mesh) >= result.model->meshes.size()) {
                hierarchyValid = false;
            } else {
                meshInstances.push_back({node.mesh, nodeDtoIndex});
            }
        }
        for (const std::int32_t child : node.children)
            visitNode(child, worldTransform, static_cast<std::int32_t>(nodeDtoIndex));
        activeNodes[nodeIndex] = false;
    };
    std::vector<std::int32_t> roots;
    const std::int32_t sceneIndex = result.model->scene >= 0 ? result.model->scene : 0;
    if (!result.model->scenes.empty() && sceneIndex >= 0 &&
        static_cast<std::size_t>(sceneIndex) < result.model->scenes.size()) {
        roots = result.model->scenes[sceneIndex].nodes;
    } else {
        std::vector<bool> isChild(result.model->nodes.size(), false);
        for (const CesiumGltf::Node& node : result.model->nodes) {
            for (const std::int32_t child : node.children) {
                if (child >= 0 && static_cast<std::size_t>(child) < isChild.size()) isChild[child] = true;
            }
        }
        for (std::size_t i = 0; i < isChild.size(); ++i) {
            if (!isChild[i]) roots.push_back(static_cast<std::int32_t>(i));
        }
    }
    for (const std::int32_t root : roots) visitNode(root, importTransform, -1);
    if (!hierarchyValid) {
        output.error.code = QStringLiteral("GLTF_NODE_HIERARCHY_INVALID");
        output.error.message = QStringLiteral("模型节点索引、变换或层级循环无效");
        return output;
    }
    if (meshInstances.empty() && result.model->nodes.empty()) {
        for (std::size_t i = 0; i < result.model->meshes.size(); ++i) {
            meshInstances.push_back(
                {static_cast<std::int32_t>(i),
                 static_cast<std::uint32_t>(meshDto->nodes.size())});
            ModelNodeDto fallbackNode;
            fallbackNode.name = QStringLiteral("Mesh %1").arg(i);
            fallbackNode.meshIndex = static_cast<std::int32_t>(i);
            for (glm::length_t column = 0; column < 4; ++column) {
                for (glm::length_t row = 0; row < 4; ++row) {
                    const std::size_t offset = static_cast<std::size_t>(column * 4 + row);
                    fallbackNode.localTransform[offset] = importTransform[column][row];
                    fallbackNode.worldTransform[offset] = importTransform[column][row];
                }
            }
            meshDto->rootNodes.push_back(static_cast<std::uint32_t>(meshDto->nodes.size()));
            meshDto->nodes.push_back(std::move(fallbackNode));
        }
    }

    for (const MeshInstance& instance : meshInstances) {
        const auto& mesh = result.model->meshes[instance.meshIndex];
        ModelNodeDto& instanceNode = meshDto->nodes[instance.nodeDtoIndex];
        glm::dmat4 instanceTransform(1.0);
        for (glm::length_t column = 0; column < 4; ++column) {
            for (glm::length_t row = 0; row < 4; ++row) {
                instanceTransform[column][row] =
                    instanceNode.worldTransform[static_cast<std::size_t>(column * 4 + row)];
            }
        }
        const glm::dmat3 normalTransform = glm::transpose(glm::inverse(glm::dmat3(instanceTransform)));
        instanceNode.firstPrimitive = static_cast<std::uint32_t>(meshDto->primitives.size());
        primitiveCount += mesh.primitives.size();
        for (const auto& primitive : mesh.primitives) {
            if (primitive.mode != CesiumGltf::MeshPrimitive::Mode::TRIANGLES) {
                continue;
            }
            const auto positionView = CesiumGltf::getPositionAccessorView(*result.model, primitive);
            const std::int64_t positionCount = std::visit(CesiumGltf::CountFromAccessor{}, positionView);
            if (positionCount <= 0) {
                continue;
            }
            const std::uint32_t vertexOffset = static_cast<std::uint32_t>(meshDto->vertices.size());
            const std::uint32_t indexOffset = static_cast<std::uint32_t>(meshDto->indices.size());
            const auto normalView = CesiumGltf::getNormalAccessorView(*result.model, primitive);
            const auto texCoordView =
                CesiumGltf::getTexCoordAccessorView(*result.model, primitive, 0);
            const auto texCoord1View =
                CesiumGltf::getTexCoordAccessorView(*result.model, primitive, 1);
            std::optional<CesiumGltf::AccessorView<
                CesiumGltf::AccessorTypes::VEC4<float>>> tangentView;
            const auto tangentAttribute = primitive.attributes.find("TANGENT");
            if (tangentAttribute != primitive.attributes.end()) {
                tangentView.emplace(*result.model, tangentAttribute->second);
                if (tangentView->status() != CesiumGltf::AccessorViewStatus::Valid ||
                    tangentView->size() != positionCount)
                    tangentView.reset();
            }
            for (std::int64_t i = 0; i < positionCount; ++i) {
                const auto position = std::visit(CesiumGltf::PositionFromAccessor{i}, positionView);
                if (!position) {
                    output.error.code = QStringLiteral("GLTF_POSITION_INVALID");
                    output.error.message = QStringLiteral("模型顶点访问器无效");
                    return output;
                }
                MeshVertexDto vertex;
                glm::dvec3 importedPosition(position->x, position->y, position->z);
                if (options.nodeMode == ModelNodeMode::Merge)
                    importedPosition = glm::dvec3(instanceTransform * glm::dvec4(importedPosition, 1.0));
                vertex.x = importedPosition.x;
                vertex.y = importedPosition.y;
                vertex.z = importedPosition.z;
                const auto normal = std::visit(CesiumGltf::NormalFromAccessor{i}, normalView);
                if (normal) {
                    glm::dvec3 importedNormal(normal->x, normal->y, normal->z);
                    if (options.nodeMode == ModelNodeMode::Merge) {
                        importedNormal = normalTransform * importedNormal;
                        const double normalLength = glm::length(importedNormal);
                        if (normalLength > 1e-12) importedNormal /= normalLength;
                    }
                    vertex.nx = importedNormal.x;
                    vertex.ny = importedNormal.y;
                    vertex.nz = importedNormal.z;
                    if (options.normalMode == ModelNormalMode::Flip) {
                        vertex.nx = -vertex.nx;
                        vertex.ny = -vertex.ny;
                        vertex.nz = -vertex.nz;
                    }
                    vertex.hasNormal = true;
                    ++normalCount;
                }
                const auto texCoord =
                    std::visit(CesiumGltf::TexCoordFromAccessor{i}, texCoordView);
                if (texCoord) {
                    vertex.u = texCoord->x;
                    vertex.v = texCoord->y;
                    vertex.hasTexCoord = true;
                    ++texCoordCount;
                }
                const auto texCoord1 =
                    std::visit(CesiumGltf::TexCoordFromAccessor{i}, texCoord1View);
                if (texCoord1) {
                    vertex.u1 = texCoord1->x;
                    vertex.v1 = texCoord1->y;
                    vertex.hasTexCoord1 = true;
                }
                if (tangentView) {
                    const auto tangent = (*tangentView)[i];
                    glm::dvec3 importedTangent(
                        tangent.value[0], tangent.value[1], tangent.value[2]);
                    if (options.nodeMode == ModelNodeMode::Merge)
                        importedTangent = normalTransform * importedTangent;
                    const double tangentLength = glm::length(importedTangent);
                    if (std::isfinite(tangentLength) && tangentLength > 1e-12 &&
                        std::isfinite(static_cast<double>(tangent.value[3]))) {
                        importedTangent /= tangentLength;
                        vertex.tx = importedTangent.x;
                        vertex.ty = importedTangent.y;
                        vertex.tz = importedTangent.z;
                        vertex.tw = tangent.value[3] < 0.0f ? -1.0 : 1.0;
                        vertex.hasTangent = true;
                    }
                }
                meshDto->vertices.push_back(vertex);
            }
            const auto indexView = CesiumGltf::getIndexAccessorView(*result.model, primitive);
            const std::int64_t indexCount = std::visit(CesiumGltf::NumIndicesFromAccessor{}, indexView);
            if (indexCount > 0) {
                for (std::int64_t i = 0; i < indexCount; ++i) {
                    const std::int64_t index = std::visit(CesiumGltf::IndexFromAccessor{i}, indexView);
                    if (index < 0 || index >= positionCount) {
                        output.error.code = QStringLiteral("GLTF_INDEX_INVALID");
                        output.error.message = QStringLiteral("模型索引超出顶点范围");
                        return output;
                    }
                    meshDto->indices.push_back(vertexOffset + static_cast<std::uint32_t>(index));
                }
            } else {
                for (std::int64_t i = 0; i < positionCount; ++i) {
                    meshDto->indices.push_back(vertexOffset + static_cast<std::uint32_t>(i));
                }
            }
            ModelPrimitiveDto primitiveDto;
            primitiveDto.firstVertex = vertexOffset;
            primitiveDto.vertexCount = static_cast<std::uint32_t>(positionCount);
            primitiveDto.firstIndex = indexOffset;
            primitiveDto.indexCount =
                static_cast<std::uint32_t>(meshDto->indices.size()) - indexOffset;
            primitiveDto.materialIndex = primitive.material;
            if (primitive.material >= 0 &&
                static_cast<std::size_t>(primitive.material) < result.model->materials.size()) {
                const auto& material = result.model->materials[primitive.material];
                primitiveDto.alphaBlend = material.alphaMode == "BLEND";
                primitiveDto.alphaMask = material.alphaMode == "MASK";
                primitiveDto.alphaCutoff = material.alphaCutoff;
                primitiveDto.doubleSided = material.doubleSided;
                const auto assignTexture = [&](std::int32_t textureIndex,
                                               std::int32_t texCoord,
                                               const QString& field,
                                               std::int32_t* outputIndex,
                                               std::int32_t* outputTexCoord) {
                    const std::int32_t supported = supportedTexCoord(
                        texCoord, field, primitive.material, &output.warnings);
                    if (supported < 0) return;
                    if (textureIndex < 0 ||
                        static_cast<std::size_t>(textureIndex) >= meshDto->textures.size() ||
                        meshDto->textures[static_cast<std::size_t>(textureIndex)].rgba8.empty()) {
                        output.warnings.append(QStringLiteral(
                            "材质 %1 的 %2 引用纹理 %3 无效或未解码，已禁用对应纹理")
                            .arg(primitive.material).arg(field).arg(textureIndex));
                        return;
                    }
                    *outputIndex = textureIndex;
                    *outputTexCoord = supported;
                };
                if (material.pbrMetallicRoughness) {
                    const auto& pbr = *material.pbrMetallicRoughness;
                    primitiveDto.metallicFactor = finiteClamped01(
                        pbr.metallicFactor, 1.0, QStringLiteral("metallicFactor"),
                        primitive.material, &output.warnings);
                    primitiveDto.roughnessFactor = finiteClamped01(
                        pbr.roughnessFactor, 1.0, QStringLiteral("roughnessFactor"),
                        primitive.material, &output.warnings);
                    if (pbr.baseColorFactor.size() >= 4) {
                        primitiveDto.baseColorR = finiteClamped01(
                            pbr.baseColorFactor[0], 1.0, QStringLiteral("baseColorFactor.r"),
                            primitive.material, &output.warnings);
                        primitiveDto.baseColorG = finiteClamped01(
                            pbr.baseColorFactor[1], 1.0, QStringLiteral("baseColorFactor.g"),
                            primitive.material, &output.warnings);
                        primitiveDto.baseColorB = finiteClamped01(
                            pbr.baseColorFactor[2], 1.0, QStringLiteral("baseColorFactor.b"),
                            primitive.material, &output.warnings);
                        primitiveDto.baseColorA = finiteClamped01(
                            pbr.baseColorFactor[3], 1.0, QStringLiteral("baseColorFactor.a"),
                            primitive.material, &output.warnings);
                    }
                    if (pbr.baseColorTexture) {
                        assignTexture(
                            pbr.baseColorTexture->index, pbr.baseColorTexture->texCoord,
                            QStringLiteral("baseColorTexture"),
                            &primitiveDto.baseColorTextureIndex,
                            &primitiveDto.baseColorTexCoord);
                    }
                    if (pbr.metallicRoughnessTexture) {
                        assignTexture(
                            pbr.metallicRoughnessTexture->index,
                            pbr.metallicRoughnessTexture->texCoord,
                            QStringLiteral("metallicRoughnessTexture"),
                            &primitiveDto.metallicRoughnessTextureIndex,
                            &primitiveDto.metallicRoughnessTexCoord);
                    }
                }
                if (material.normalTexture) {
                    assignTexture(
                        material.normalTexture->index, material.normalTexture->texCoord,
                        QStringLiteral("normalTexture"), &primitiveDto.normalTextureIndex,
                        &primitiveDto.normalTexCoord);
                    primitiveDto.normalScale = finiteNonNegative(
                        material.normalTexture->scale, 1.0, QStringLiteral("normalTexture.scale"),
                        primitive.material, &output.warnings);
                }
                if (material.occlusionTexture) {
                    assignTexture(
                        material.occlusionTexture->index, material.occlusionTexture->texCoord,
                        QStringLiteral("occlusionTexture"),
                        &primitiveDto.occlusionTextureIndex,
                        &primitiveDto.occlusionTexCoord);
                    primitiveDto.occlusionStrength = finiteClamped01(
                        material.occlusionTexture->strength, 1.0,
                        QStringLiteral("occlusionTexture.strength"), primitive.material,
                        &output.warnings);
                }
                if (material.emissiveTexture) {
                    assignTexture(
                        material.emissiveTexture->index, material.emissiveTexture->texCoord,
                        QStringLiteral("emissiveTexture"),
                        &primitiveDto.emissiveTextureIndex,
                        &primitiveDto.emissiveTexCoord);
                }
                if (material.emissiveFactor.size() >= 3) {
                    primitiveDto.emissiveR = finiteNonNegative(
                        material.emissiveFactor[0], 0.0, QStringLiteral("emissiveFactor.r"),
                        primitive.material, &output.warnings);
                    primitiveDto.emissiveG = finiteNonNegative(
                        material.emissiveFactor[1], 0.0, QStringLiteral("emissiveFactor.g"),
                        primitive.material, &output.warnings);
                    primitiveDto.emissiveB = finiteNonNegative(
                        material.emissiveFactor[2], 0.0, QStringLiteral("emissiveFactor.b"),
                        primitive.material, &output.warnings);
                }
            }
            meshDto->primitives.push_back(primitiveDto);
            vertexCount += static_cast<std::uint64_t>(positionCount);
        }
        instanceNode.primitiveCount =
            static_cast<std::uint32_t>(meshDto->primitives.size()) - instanceNode.firstPrimitive;
    }
    if (options.nodeMode == ModelNodeMode::Merge) {
        ModelNodeDto mergedRoot;
        mergedRoot.name = QStringLiteral("合并模型");
        mergedRoot.firstPrimitive = 0;
        mergedRoot.primitiveCount = static_cast<std::uint32_t>(meshDto->primitives.size());
        meshDto->nodes.clear();
        meshDto->nodes.push_back(std::move(mergedRoot));
        meshDto->rootNodes = {0};
    }
    if (meshDto->vertices.empty() || meshDto->indices.empty()) {
        output.error.code = QStringLiteral("GLTF_TRIANGLES_MISSING");
        output.error.message = QStringLiteral("模型不包含可导入的三角形几何");
        return output;
    }
    if (options.normalMode == ModelNormalMode::Recalculate) {
        for (MeshVertexDto& vertex : meshDto->vertices) {
            vertex.nx = 0.0;
            vertex.ny = 0.0;
            vertex.nz = 0.0;
            vertex.hasNormal = false;
        }
        for (const ModelPrimitiveDto& primitive : meshDto->primitives) {
            const std::uint32_t end = primitive.firstIndex + primitive.indexCount;
            for (std::uint32_t index = primitive.firstIndex; index + 2 < end; index += 3) {
                const std::uint32_t ia = meshDto->indices[index];
                const std::uint32_t ib = meshDto->indices[index + 1];
                const std::uint32_t ic = meshDto->indices[index + 2];
                if (ia >= meshDto->vertices.size() || ib >= meshDto->vertices.size() ||
                    ic >= meshDto->vertices.size()) continue;
                const MeshVertexDto& a = meshDto->vertices[ia];
                const MeshVertexDto& b = meshDto->vertices[ib];
                const MeshVertexDto& c = meshDto->vertices[ic];
                const glm::dvec3 face = glm::cross(
                    glm::dvec3(b.x - a.x, b.y - a.y, b.z - a.z),
                    glm::dvec3(c.x - a.x, c.y - a.y, c.z - a.z));
                for (const std::uint32_t vertexIndex : {ia, ib, ic}) {
                    MeshVertexDto& vertex = meshDto->vertices[vertexIndex];
                    vertex.nx += face.x;
                    vertex.ny += face.y;
                    vertex.nz += face.z;
                }
            }
        }
        normalCount = 0;
        for (MeshVertexDto& vertex : meshDto->vertices) {
            glm::dvec3 normal(vertex.nx, vertex.ny, vertex.nz);
            const double length = glm::length(normal);
            normal = length > 1e-12 ? normal / length : glm::dvec3(0.0, 1.0, 0.0);
            vertex.nx = normal.x;
            vertex.ny = normal.y;
            vertex.nz = normal.z;
            vertex.hasNormal = true;
            ++normalCount;
        }
    }
    std::uint64_t pbrPrimitiveCount = 0;
    std::uint64_t metallicRoughnessTextureCount = 0;
    std::uint64_t normalTextureCount = 0;
    std::uint64_t occlusionTextureCount = 0;
    std::uint64_t emissiveTextureCount = 0;
    std::uint64_t explicitTangentPrimitiveCount = 0;
    std::uint64_t generatedTangentPrimitiveCount = 0;
    std::uint64_t materialFallbackCount = 0;
    for (ModelPrimitiveDto& primitive : meshDto->primitives) {
        if (primitive.materialIndex >= 0) ++pbrPrimitiveCount;
        if (primitive.metallicRoughnessTextureIndex >= 0) ++metallicRoughnessTextureCount;
        if (primitive.occlusionTextureIndex >= 0) ++occlusionTextureCount;
        if (primitive.emissiveTextureIndex >= 0) ++emissiveTextureCount;
        if (primitive.normalTextureIndex < 0) continue;
        ++normalTextureCount;
        QString tangentFailure;
        if (preparePrimitiveTangents(
                meshDto.get(), &primitive, primitive.normalTexCoord, &tangentFailure)) {
            if (primitive.tangentSource == ModelTangentSource::Explicit)
                ++explicitTangentPrimitiveCount;
            else if (primitive.tangentSource == ModelTangentSource::Generated)
                ++generatedTangentPrimitiveCount;
            continue;
        }
        primitive.normalMappingEnabled = false;
        primitive.tangentSource = ModelTangentSource::None;
        ++materialFallbackCount;
        output.warnings.append(QStringLiteral(
            "材质 %1 的法线贴图已禁用：%2")
            .arg(primitive.materialIndex).arg(tangentFailure));
    }
    QVariantMap statistics;
    statistics.insert(QStringLiteral("meshes"), static_cast<qulonglong>(result.model->meshes.size()));
    statistics.insert(QStringLiteral("primitives"), static_cast<qulonglong>(primitiveCount));
    statistics.insert(QStringLiteral("vertices"), static_cast<qulonglong>(vertexCount));
    statistics.insert(QStringLiteral("materials"), static_cast<qulonglong>(result.model->materials.size()));
    statistics.insert(QStringLiteral("warnings"), output.warnings.size());
    statistics.insert(QStringLiteral("unsupportedUsedExtensions"),
                      unsupportedUsedExtensionCount);
    statistics.insert(QStringLiteral("gltfExtensionsUsed"), extensionDeclarations.used);
    statistics.insert(QStringLiteral("gltfExtensionsRequired"),
                      extensionDeclarations.required);
    statistics.insert(QStringLiteral("indices"), static_cast<qulonglong>(meshDto->indices.size()));
    statistics.insert(QStringLiteral("normals"), static_cast<qulonglong>(normalCount));
    statistics.insert(QStringLiteral("texcoords"), static_cast<qulonglong>(texCoordCount));
    statistics.insert(QStringLiteral("pbrPrimitives"),
                      static_cast<qulonglong>(pbrPrimitiveCount));
    statistics.insert(QStringLiteral("metallicRoughnessTextures"),
                      static_cast<qulonglong>(metallicRoughnessTextureCount));
    statistics.insert(QStringLiteral("normalTextures"),
                      static_cast<qulonglong>(normalTextureCount));
    statistics.insert(QStringLiteral("occlusionTextures"),
                      static_cast<qulonglong>(occlusionTextureCount));
    statistics.insert(QStringLiteral("emissiveTextures"),
                      static_cast<qulonglong>(emissiveTextureCount));
    statistics.insert(QStringLiteral("explicitTangentPrimitives"),
                      static_cast<qulonglong>(explicitTangentPrimitiveCount));
    statistics.insert(QStringLiteral("generatedTangentPrimitives"),
                      static_cast<qulonglong>(generatedTangentPrimitiveCount));
    statistics.insert(QStringLiteral("materialFallbacks"),
                      static_cast<qulonglong>(materialFallbackCount));
    std::uint64_t decodedTextures = 0;
    std::uint64_t textureBytes = 0;
    for (const ModelTextureDto& texture : meshDto->textures) {
        if (!texture.rgba8.empty()) {
            ++decodedTextures;
            textureBytes += texture.rgba8.size();
        }
    }
    statistics.insert(QStringLiteral("decodedTextures"), static_cast<qulonglong>(decodedTextures));
    statistics.insert(QStringLiteral("textureBytes"), static_cast<qulonglong>(textureBytes));
    statistics.insert(QStringLiteral("textureFallbacks"), textureFallbackCount);
    statistics.insert(QStringLiteral("nodes"), static_cast<qulonglong>(meshDto->nodes.size()));
    statistics.insert(QStringLiteral("meshInstances"), static_cast<qulonglong>(meshInstances.size()));
    statistics.insert(QStringLiteral("unitScale"), options.unitScale);
    statistics.insert(QStringLiteral("sourceUpAxis"),
                      upAxis == ModelSourceUpAxis::ZUp
                          ? QStringLiteral("Z") : QStringLiteral("Y"));
    statistics.insert(QStringLiteral("normalMode"),
                      options.normalMode == ModelNormalMode::Recalculate
                          ? QStringLiteral("recalculate")
                          : options.normalMode == ModelNormalMode::Flip
                              ? QStringLiteral("flip") : QStringLiteral("read"));
    statistics.insert(QStringLiteral("nodeMode"),
                      options.nodeMode == ModelNodeMode::Merge
                          ? QStringLiteral("merge") : QStringLiteral("preserve"));
    meshDto->rebuildRayIndex(options.rayIndexLeafSize);
    statistics.insert(QStringLiteral("rayIndexLeafSize"),
                      static_cast<qulonglong>(meshDto->rayIndexLeafSize));
    statistics.insert(QStringLiteral("rayIndexTriangles"),
                      static_cast<qulonglong>(meshDto->rayTriangles.size()));
    statistics.insert(QStringLiteral("rayIndexNodes"),
                      static_cast<qulonglong>(meshDto->rayNodes.size()));
    output.statistics = statistics;
    output.success = true;
    output.mesh = std::move(meshDto);
    return output;
}

} // namespace View3DModel

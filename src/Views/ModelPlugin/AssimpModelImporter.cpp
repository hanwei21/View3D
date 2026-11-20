#include "AssimpModelImporter.h"

#include "ThreeDsKeyframeStream.h"

#include <assimp/Importer.hpp>
#include <assimp/cimport.h>
#include <assimp/importerdesc.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QRegularExpression>
#include <QtGlobal>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

namespace View3DModel {
namespace {

std::array<double, 16> matrixFromAi(const aiMatrix4x4& m) {
    return {
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4};
}

struct AssimpNodeRecord {
    ModelNodeDto dto;
    std::vector<std::uint32_t> primitiveIndices;
};

ModelReadResult failure(const QString& code, const QString& message) {
    ModelReadResult result;
    result.error.code = code;
    result.error.message = message;
    return result;
}

// Assimp 的 Blender 读取器只认未压缩或 gzip 的旧版文件。Blender 3.0 起默认
// zstd 压缩，4.x 的 DNA 结构也不再兼容（上游已把 BLEND 标记为 deprecated），
// 提前给出可操作的提示，别让用户看到 "BLENDER magic bytes are missing"。
QString blendUnsupportedReason(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray header = file.read(12);
    if (header.startsWith(QByteArray::fromHex("28b52ffd"))) {
        return QStringLiteral(
            "该 .blend 使用 zstd 压缩（Blender 3.0 起的默认保存方式），Assimp 无法读取；"
            "请导出为 glTF/FBX/OBJ 再导入，或在 Blender 3.x 中取消“压缩文件”后另存");
    }
    if (header.size() == 12 && header.startsWith(QByteArrayLiteral("BLENDER"))) {
        const int major = QString::fromLatin1(header.mid(9, 1)).toInt();
        if (major >= 4) {
            return QStringLiteral(
                "Assimp 不支持 Blender %1.%2 的 .blend 数据结构；"
                "请导出为 glTF/FBX/OBJ 再导入")
                .arg(major)
                .arg(QString::fromLatin1(header.mid(10, 2)).toInt());
        }
    }
    return {};
}

// 3DS 一类格式常把贴图名写成 8~12 字符且不带扩展名（"Asphalt_"、"Fencin03"），
// 而成果目录里的实际文件是 Asphalt_.jpg / Fencin03.png，大小写也可能不一致。
// 所以按「原样 → 补常见扩展名 → 目录内大小写无关匹配」逐级找。
class TextureFileResolver {
  public:
    TextureFileResolver(QString modelDirectory, QStringList extraDirectories)
        : m_directories { std::move(modelDirectory) }
    {
        for (const QString& extra : extraDirectories)
            if (!extra.isEmpty() && !m_directories.contains(extra))
                m_directories.push_back(extra);
    }

    QString resolve(const QString& rawName)
    {
        const QString trimmed = rawName.trimmed();
        if (trimmed.isEmpty())
            return {};
        const QString fileName = QFileInfo(trimmed).fileName();
        if (fileName.isEmpty())
            return {};
        QStringList candidates { fileName };
        if (QFileInfo(fileName).suffix().isEmpty()) {
            static const QStringList extensions {QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
                                                 QStringLiteral(".png"), QStringLiteral(".bmp"),
                                                 QStringLiteral(".tga"), QStringLiteral(".gif"),
                                                 QStringLiteral(".tif"), QStringLiteral(".dds")};
            for (const QString& extension : extensions)
                candidates.push_back(fileName + extension);
        }
        for (const QString& directory : m_directories) {
            if (directory.isEmpty())
                continue;
            for (const QString& candidate : candidates) {
                const QString path = QDir(directory).filePath(candidate);
                if (QFileInfo::exists(path))
                    return QFileInfo(path).absoluteFilePath();
            }
            // 大小写兜底：导出器写出的大小写经常和实际文件名不一致。
            const QString matched = matchIgnoringCase(directory, fileName);
            if (!matched.isEmpty())
                return matched;
        }
        return {};
    }

  private:
    QString matchIgnoringCase(const QString& directory, const QString& fileName)
    {
        const QHash<QString, QString>& entries = listingFor(directory);
        const auto found = entries.constFind(fileName.toLower());
        if (found == entries.constEnd())
            return {};
        return QDir(directory).filePath(found.value());
    }

    // 目录清单按目录缓存；返回引用安全，因为缓存本体是成员。
    const QHash<QString, QString>& listingFor(const QString& directory)
    {
        const auto cached = m_listings.constFind(directory);
        if (cached != m_listings.constEnd())
            return *cached;
        QHash<QString, QString> entries;
        const QFileInfoList files =
            QDir(directory).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::NoSort);
        for (const QFileInfo& file : files)
            entries.insert(file.fileName().toLower(), file.fileName());
        return *m_listings.insert(directory, entries);
    }

    QStringList m_directories;
    QHash<QString, QHash<QString, QString>> m_listings;
};

// 解码成 RGBA8 存进 DTO；同一路径只解一次。返回 mesh.textures 里的下标，失败给 -1。
std::int32_t loadTexture(View3D::ModelMeshDto& mesh, QHash<QString, std::int32_t>& byPath,
                         const QString& filePath)
{
    const auto existing = byPath.constFind(filePath);
    if (existing != byPath.cend())
        return existing.value();
    QImage image;
    if (!image.load(filePath) || image.width() <= 0 || image.height() <= 0) {
        byPath.insert(filePath, -1);
        return -1;
    }
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    View3D::ModelTextureDto textureDto;
    textureDto.width = rgba.width();
    textureDto.height = rgba.height();
    textureDto.rgba8.resize(static_cast<std::size_t>(textureDto.width) *
                            static_cast<std::size_t>(textureDto.height) * 4u);
    for (int row = 0; row < rgba.height(); ++row) {
        const uchar* source = rgba.constScanLine(row);
        std::byte* target = textureDto.rgba8.data() +
                            static_cast<std::size_t>(row) *
                                static_cast<std::size_t>(rgba.width()) * 4u;
        std::memcpy(target, source, static_cast<std::size_t>(rgba.width()) * 4u);
    }
    mesh.textures.push_back(std::move(textureDto));
    const std::int32_t index = static_cast<std::int32_t>(mesh.textures.size()) - 1;
    byPath.insert(filePath, index);
    return index;
}

bool nearBlack(const aiColor4D& color)
{
    return color.r <= 0.01 && color.g <= 0.01 && color.b <= 0.01;
}

} // namespace

const QStringList& AssimpModelImporter::supportedSuffixes() {    static const QStringList suffixes = [] {
        QStringList list;
        const std::size_t count = aiGetImportFormatCount();
        for (std::size_t i = 0; i < count; ++i) {
            const aiImporterDesc* desc = aiGetImportFormatDescription(i);
            if (!desc || !desc->mFileExtensions)
                continue;
            const QString raw = QString::fromLatin1(desc->mFileExtensions);
            const QStringList tokens =
                raw.split(QRegularExpression(QStringLiteral("[ ;]+")), Qt::SkipEmptyParts);
            for (const QString& token : tokens) {
                QString suffix = token;
                suffix.remove(QLatin1Char('*'));
                suffix.remove(QLatin1Char('.'));
                suffix = suffix.toLower();
                // glTF/glb 由 Cesium 的读取器负责，.json 既可能是文本 glTF 也可能
                // 是 3D Tiles 清单，两种情况都不能靠 assimp 抢下来。
                if (suffix.isEmpty() || suffix == QLatin1String("gltf") ||
                    suffix == QLatin1String("glb") || suffix == QLatin1String("json"))
                    continue;
                if (!list.contains(suffix))
                    list.append(suffix);
            }
        }
        list.sort(Qt::CaseInsensitive);
        return list;
    }();
    return suffixes;
}

bool AssimpModelImporter::supportsExtension(const QString& suffix) {
    return supportedSuffixes().contains(suffix.toLower());
}

ModelReadResult AssimpModelImporter::importFile(const QString& path,
                                                  const ModelImportOptions& options) const
{
    if (!std::isfinite(options.unitScale) || options.unitScale <= 0.0) {
        return failure(QStringLiteral("MODEL_IMPORT_OPTIONS_INVALID"),
                       QStringLiteral("模型单位比例无效"));
    }
    const QFileInfo fileInfo(path);
    if (fileInfo.suffix().compare(QLatin1String("blend"), Qt::CaseInsensitive) == 0) {
        const QString reason = blendUnsupportedReason(fileInfo.absoluteFilePath());
        if (!reason.isEmpty())
            return failure(QStringLiteral("MODEL_FORMAT_UNSUPPORTED"), reason);
    }
    const std::string nativePath = fileInfo.absoluteFilePath().toStdString();
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, 45.0f);
    // .3ds 的 keyframer 节点层级会让 Assimp 沿父链无上限递归，1 MB 线程栈直接溢出
    // （进程连错误框都来不及弹）；这里给它一份摘掉 keyframer 的虚拟文件。判定条件
    // 不干净时按原样读，绝不把一个本来能读的文件改坏。VIEW3D_3DS_KEEP_KEYFRAMES=1
    // 用来关掉这条通路做 A/B 对照。
    if (fileInfo.suffix().compare(QLatin1String("3ds"), Qt::CaseInsensitive) == 0 &&
        qEnvironmentVariableIntValue("VIEW3D_3DS_KEEP_KEYFRAMES") <= 0) {
        const ThreeDsKeyframeStrip strip = planThreeDsKeyframeStrip(fileInfo.absoluteFilePath());
        if (strip.applies) {
            // 按 Importer.hpp 的约定，SetIOHandler 之后所有权归 Importer，
            // 它析构时 delete；这里再包一层 unique_ptr 就是双重释放。
            importer.SetIOHandler(new ThreeDsKeyframeIOSystem(nativePath, strip));
            std::cerr << "VIEW3D_3DS_KEYFRAME_SKIPPED dropped_mb="
                      << strip.droppedBytes / 1048576 << " virtual_mb="
                      << strip.virtualSize / 1048576 << std::endl;
        } else {
            std::cerr << "VIEW3D_3DS_KEYFRAME_KEPT reason=" << strip.note.toUtf8().constData()
                      << std::endl;
        }
    }
    const aiScene* scene = importer.ReadFile(
        nativePath,
        aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices |
            aiProcess_ImproveCacheLocality | aiProcess_OptimizeMeshes);
    if (!scene || !scene->HasMeshes()) {
        return failure(QStringLiteral("MODEL_PARSE_FAILED"),
                       QStringLiteral("Assimp 无法解析模型：%1")
                           .arg(QString::fromUtf8(importer.GetErrorString())));
    }

    // 预算检查放在 DTO 转换之前：一个顶点 128 字节，等转完再拒绝已经白吃了内存。
    std::uint64_t totalVertices = 0;
    for (std::uint32_t meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (mesh)
            totalVertices += mesh->mNumVertices;
    }
    if (options.vertexBudget > 0 && totalVertices > options.vertexBudget) {
        return failure(QStringLiteral("MODEL_VERTEX_BUDGET_EXCEEDED"),
                       QStringLiteral("模型有 %1 个顶点，超过单次导入上限 %2：请在源软件里合并或减面")
                           .arg(totalVertices)
                           .arg(options.vertexBudget));
    }
    // 大模型的失败往往出在读取之后的转换/上屏，光看 SUBMIT 分不出死在哪一段。
    // 这里把 Assimp 交回来的规模先钉在日志里，后面每一步都能对着它排。
    std::cerr << "VIEW3D_ASSIMP_READ meshes=" << scene->mNumMeshes << " materials="
              << scene->mNumMaterials << " vertices=" << totalVertices
              << " nodes=" << (scene->mRootNode ? scene->mRootNode->mNumChildren : 0) << std::endl;

    auto meshDto = std::make_shared<ModelMeshDto>();
    QStringList resultWarnings;
    // 材质外观表：diffuse 颜色与贴图一次算好，网格循环只查表。
    // 3DS 这类导出器把颜色全放在贴图里、diffuse 写成黑，而 PBS 着色是「贴图 × diffuse」，
    // 黑 diffuse 会把贴图整个乘没——厂区模型里那些该有颜色的块就是这么变成纯黑的。
    // 所以带贴图且 diffuse 近黑时按白处理。
    struct Appearance {
        double r { 1.0 };
        double g { 1.0 };
        double b { 1.0 };
        double a { 1.0 };
        std::int32_t texture { -1 };
        double roughness { 0.9 };
    };
    std::vector<Appearance> appearances(scene->mNumMaterials);
    {
        TextureFileResolver resolver(fileInfo.absolutePath(), options.textureSearchPaths);
        QHash<QString, std::int32_t> textureByPath;
        int referenced = 0;
        int missing = 0;
        int blackWithoutTexture = 0;
        int ambientAvailable = 0;
        int specularAvailable = 0;
        int blackFlat = 0, blackFlatAmbient = 0, blackFlatSpecular = 0;
        QStringList missingNames;
        for (std::uint32_t materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex) {
            const aiMaterial* material = scene->mMaterials[materialIndex];
            if (!material)
                continue;
            Appearance look;
            aiColor4D diffuse(1.0f, 1.0f, 1.0f, 1.0f);
            if (material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS) {
                look.r = diffuse.r;
                look.g = diffuse.g;
                look.b = diffuse.b;
                look.a = diffuse.a;
            }
            // 传统材质（3DS/OBJ/FBX）没有金属度概念，DTO 默认的 metallic=1 会让 PBS
            // 走全金属工作流：无漫反射、颜色全靠环境镜面反射，而本场景没有 IBL 环境
            // 贴图，结果整片纯黑。统一按非金属处理，粗糙度由高光指数换算，与
            // OsgModelImporter 同一约定。
            float shininess = 0.0f;
            if (material->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS &&
                std::isfinite(shininess))
                look.roughness = std::clamp(1.0 - static_cast<double>(shininess) / 128.0,
                                            0.05, 1.0);
            aiString texturePath;
            if (material->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath) == AI_SUCCESS &&
                texturePath.length > 0) {
                ++referenced;
                const QString rawName = QString::fromUtf8(texturePath.C_Str());
                const QString resolved = resolver.resolve(rawName);
                if (resolved.isEmpty()) {
                    ++missing;
                    if (missingNames.size() < 12)
                        missingNames.push_back(rawName);
                    // 贴图缺文件时，材质里还有没有别的颜色可用？这决定要不要兜底。
                    aiColor4D other(0.0f, 0.0f, 0.0f, 1.0f);
                    if (nearBlack(diffuse)) {
                        ++blackWithoutTexture;
                        if (material->Get(AI_MATKEY_COLOR_AMBIENT, other) == AI_SUCCESS &&
                            !nearBlack(other))
                            ++ambientAvailable;
                        if (material->Get(AI_MATKEY_COLOR_SPECULAR, other) == AI_SUCCESS &&
                            !nearBlack(other))
                            ++specularAvailable;
                    }
                } else {
                    const std::int32_t index = loadTexture(*meshDto, textureByPath, resolved);
                    if (index >= 0) {
                        look.texture = index;
                        if (nearBlack(diffuse)) {
                            look.r = 1.0;
                            look.g = 1.0;
                            look.b = 1.0;
                        }
                    } else {
                        ++missing;
                        if (missingNames.size() < 12)
                            missingNames.push_back(rawName + QStringLiteral("（解码失败）"));
                    }
                }
            }
            if (nearBlack(diffuse) && look.texture < 0) {
                // 参考查看器（DasViewer/OSG）走经典 OpenGL 光照：最终颜色含
                // ambient×环境光 + specular×高光 项，diffuse 黑但 ambient/specular
                // 有色的物体仍可见。PBS 只把 baseColor 当 diffuse，这些材质就会整片
                // 纯黑（厂区里 Foreground/Color_04 等默认材质覆盖了大量基元）。这里按
                // ambient→specular 顺序把可用颜色折进 baseColor；各通道全黑才保留黑。
                ++blackFlat;
                aiColor4D amb(0, 0, 0, 1), spec(0, 0, 0, 1);
                material->Get(AI_MATKEY_COLOR_AMBIENT, amb);
                material->Get(AI_MATKEY_COLOR_SPECULAR, spec);
                if (!nearBlack(amb)) {
                    look.r = amb.r; look.g = amb.g; look.b = amb.b;
                    ++blackFlatAmbient;
                } else if (!nearBlack(spec)) {
                    look.r = spec.r; look.g = spec.g; look.b = spec.b;
                    ++blackFlatSpecular;
                }
            }
            appearances[materialIndex] = look;
        }
        std::cerr << "VIEW3D_ASSIMP_MATERIALS referenced=" << referenced
                  << " loaded=" << static_cast<int>(meshDto->textures.size())
                  << " missing=" << missing
                  << " blackNoTexture=" << blackWithoutTexture
                  << " ambientAvailable=" << ambientAvailable
                  << " specularAvailable=" << specularAvailable
                  << " blackFlat=" << blackFlat
                  << " recoveredAmbient=" << blackFlatAmbient
                  << " recoveredSpecular=" << blackFlatSpecular << std::endl;
        if (missing > 0) {
            // 找不到贴图时这些材质的 diffuse 往往是黑的，画面就会留下纯黑块；
            // 把名字列出来，用户能在源软件里对照着装贴图目录。
            resultWarnings.push_back(QStringLiteral("%1 个材质引用的贴图找不到：%2%3")
                                         .arg(missing)
                                         .arg(missingNames.join(QStringLiteral("、")))
                                         .arg(missing > missingNames.size()
                                                  ? QStringLiteral(" 等")
                                                  : QString()));
        }
    }
    std::uint64_t vertexCount = 0;
    std::uint64_t indexCount = 0;
    std::uint64_t primitiveCount = 0;
    QHash<std::uint32_t, std::uint32_t> meshToPrimitive;

    for (std::uint32_t meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (!mesh || mesh->mNumVertices == 0)
            continue;
        const std::uint32_t firstVertex = static_cast<std::uint32_t>(meshDto->vertices.size());
        const double scale = options.unitScale;
        for (std::uint32_t v = 0; v < mesh->mNumVertices; ++v) {
            MeshVertexDto vertex;
            vertex.x = mesh->mVertices[v].x * scale;
            vertex.y = mesh->mVertices[v].y * scale;
            vertex.z = mesh->mVertices[v].z * scale;
            if (options.sourceUpAxis == ModelSourceUpAxis::ZUp) {
                const double y = vertex.y;
                vertex.y = vertex.z;
                vertex.z = -y;
            }
            if (mesh->HasNormals()) {
                vertex.nx = mesh->mNormals[v].x;
                vertex.ny = mesh->mNormals[v].y;
                vertex.nz = mesh->mNormals[v].z;
                if (options.sourceUpAxis == ModelSourceUpAxis::ZUp) {
                    const double ny = vertex.ny;
                    vertex.ny = vertex.nz;
                    vertex.nz = -ny;
                }
                vertex.hasNormal = true;
            }
            if (mesh->HasTextureCoords(0)) {
                vertex.u = mesh->mTextureCoords[0][v].x;
                vertex.v = mesh->mTextureCoords[0][v].y;
                vertex.hasTexCoord = true;
            }
            if (mesh->HasVertexColors(0)) {
                const aiColor4D& color = mesh->mColors[0][v];
                vertex.hasColor = true;
                vertex.colorR = static_cast<std::uint8_t>(std::lround(color.r * 255.0));
                vertex.colorG = static_cast<std::uint8_t>(std::lround(color.g * 255.0));
                vertex.colorB = static_cast<std::uint8_t>(std::lround(color.b * 255.0));
                vertex.colorA = static_cast<std::uint8_t>(std::lround(color.a * 255.0));
            }
            meshDto->vertices.push_back(vertex);
        }
        const std::uint32_t firstIndex = static_cast<std::uint32_t>(meshDto->indices.size());
        std::uint32_t indexCountForMesh = 0;
        for (std::uint32_t f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            for (std::uint32_t i = 0; i < face.mNumIndices; ++i)
                meshDto->indices.push_back(firstVertex + face.mIndices[i]);
            indexCountForMesh += face.mNumIndices;
        }
        ModelPrimitiveDto primitive;
        primitive.firstVertex = firstVertex;
        primitive.vertexCount = static_cast<std::uint32_t>(mesh->mNumVertices);
        primitive.firstIndex = firstIndex;
        primitive.indexCount = indexCountForMesh;
        primitive.materialIndex = mesh->mMaterialIndex >= 0
                                      ? static_cast<std::int32_t>(mesh->mMaterialIndex)
                                      : -1;
        // 只有顶点没有面的网格就是点云（PLY / 顶点云 OBJ），不能当成空网格丢掉。
        primitive.topology = mesh->HasFaces() ? ModelPrimitiveTopology::Triangles
                                              : ModelPrimitiveTopology::Points;
        primitive.metallicFactor = 0.0;
        primitive.roughnessFactor = 0.9;
        if (mesh->mMaterialIndex >= 0 &&
            static_cast<std::uint32_t>(mesh->mMaterialIndex) < scene->mNumMaterials) {
            const Appearance& look = appearances[static_cast<std::size_t>(mesh->mMaterialIndex)];
            primitive.baseColorR = look.r;
            primitive.baseColorG = look.g;
            primitive.baseColorB = look.b;
            primitive.baseColorA = look.a;
            primitive.roughnessFactor = look.roughness;
            // 没有 UV 的网格不能挂贴图：PBS 会因「UV set #0 少于 2 个坐标」直接抛异常。
            if (look.texture >= 0 && mesh->HasTextureCoords(0)) {
                primitive.baseColorTextureIndex = look.texture;
                primitive.baseColorTexCoord = 0;
            }
        }
        meshDto->primitives.push_back(primitive);
        meshToPrimitive.insert(meshIndex, static_cast<std::uint32_t>(meshDto->primitives.size()) - 1);
        vertexCount += mesh->mNumVertices;
        indexCount += indexCountForMesh;
        ++primitiveCount;
    }

    if (meshDto->vertices.empty() || meshDto->primitives.empty()) {
        return failure(QStringLiteral("MODEL_EMPTY_MESH"), QStringLiteral("模型没有可用几何"));
    }

    std::uint32_t nodeIndex = 0;
    std::vector<AssimpNodeRecord> records;
    records.reserve(scene->mRootNode ? scene->mRootNode->mNumChildren + 1 : 1);
    // 展平成 records（前序、孩子正序），顺序与旧的递归实现逐字一致。
    // 用显式栈而不是递归：3DS 这类层级能退化到上万深，1 MB 线程栈扛不住。
    struct PendingNode {
        const aiNode* node;
        std::int32_t parentIndex;
    };
    std::vector<PendingNode> pending;
    if (scene->mRootNode)
        pending.push_back({scene->mRootNode, -1});
    while (!pending.empty()) {
        const PendingNode current = pending.back();
        pending.pop_back();
        const aiNode* node = current.node;
        if (!node)
            continue;
        const std::int32_t parentIndex = current.parentIndex;
        AssimpNodeRecord record;
        record.dto.name = QString::fromUtf8(node->mName.C_Str());
        record.dto.sourceNodeIndex = static_cast<std::int32_t>(nodeIndex);
        record.dto.parentIndex = parentIndex;
        record.dto.localTransform = matrixFromAi(node->mTransformation);
        if (parentIndex >= 0 && parentIndex < static_cast<std::int32_t>(records.size())) {
            const auto& parent = records[static_cast<std::size_t>(parentIndex)].dto;
            const auto& local = record.dto.localTransform;
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col) {
                    double value = 0.0;
                    for (int k = 0; k < 4; ++k)
                        value += parent.worldTransform[row * 4 + k] * local[k * 4 + col];
                    record.dto.worldTransform[row * 4 + col] = value;
                }
        } else {
            record.dto.worldTransform = record.dto.localTransform;
        }
        for (std::uint32_t m = 0; m < node->mNumMeshes; ++m) {
            const auto it = meshToPrimitive.constFind(node->mMeshes[m]);
            if (it != meshToPrimitive.cend())
                record.primitiveIndices.push_back(it.value());
        }
        const std::uint32_t recordIndex = nodeIndex++;
        records.push_back(std::move(record));
        for (std::uint32_t c = node->mNumChildren; c > 0; --c)
            pending.push_back({node->mChildren[c - 1], static_cast<std::int32_t>(recordIndex)});
    }

    for (std::size_t i = 0; i < records.size(); ++i) {
        AssimpNodeRecord& record = records[i];
        if (!record.primitiveIndices.empty()) {
            record.dto.firstPrimitive = record.primitiveIndices.front();
            record.dto.primitiveCount = static_cast<std::uint32_t>(
                record.primitiveIndices.size());
            record.dto.meshIndex = static_cast<std::int32_t>(record.primitiveIndices.front());
        }
        for (std::size_t child = 0; child < records.size(); ++child) {
            if (records[child].dto.parentIndex == static_cast<std::int32_t>(i))
                record.dto.children.push_back(static_cast<std::uint32_t>(child));
        }
        meshDto->nodes.push_back(record.dto);
    }
    for (std::size_t i = 0; i < records.size(); ++i)
        if (records[i].dto.parentIndex < 0)
            meshDto->rootNodes.push_back(static_cast<std::uint32_t>(i));

    meshDto->rebuildRayIndex(options.rayIndexLeafSize);
    QVariantMap statistics;
    statistics.insert(QStringLiteral("meshes"), static_cast<qulonglong>(scene->mNumMeshes));
    statistics.insert(QStringLiteral("primitives"), static_cast<qulonglong>(primitiveCount));
    statistics.insert(QStringLiteral("vertices"), static_cast<qulonglong>(vertexCount));
    statistics.insert(QStringLiteral("indices"), static_cast<qulonglong>(indexCount));
    statistics.insert(QStringLiteral("materials"), static_cast<qulonglong>(scene->mNumMaterials));
    statistics.insert(QStringLiteral("textures"),
                      static_cast<qulonglong>(meshDto->textures.size()));
    statistics.insert(QStringLiteral("unitScale"), options.unitScale);
    statistics.insert(QStringLiteral("sourceUpAxis"), QStringLiteral("Y"));
    statistics.insert(QStringLiteral("nodeMode"),
                      options.nodeMode == ModelNodeMode::Merge
                          ? QStringLiteral("merge")
                          : QStringLiteral("preserve"));

    ModelReadResult output;
    output.statistics = statistics;
    output.success = true;
    output.warnings = resultWarnings;
    output.mesh = std::move(meshDto);
    return output;
}

} // namespace View3DModel

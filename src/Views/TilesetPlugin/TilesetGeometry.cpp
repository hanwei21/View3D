#include "TilesetGeometry.h"

#include <CesiumGltf/AccessorUtility.h>
#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/ExtensionExtMeshGpuInstancing.h>
#include <CesiumGltf/ExtensionKhrMaterialsUnlit.h>
#include <CesiumGltf/Image.h>
#include <CesiumGltf/Material.h>
#include <CesiumGltf/Mesh.h>
#include <CesiumGltf/MeshPrimitive.h>
#include <CesiumGltf/Model.h>
#include <CesiumGltf/Node.h>
#include <CesiumGltf/Sampler.h>
#include <CesiumGltf/Texture.h>
#include <CesiumGltfContent/GltfUtilities.h>
#include <CesiumImage/ImageAsset.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace View3DTiles {
namespace {

using namespace CesiumGltf;

template <typename T> T clampNumber(T value, T fallback, T minimum, T maximum)
{
    if (!std::isfinite(static_cast<double>(value)))
        return fallback;
    return std::clamp(value, minimum, maximum);
}

void assignTransform(std::array<double, 16>& target, const glm::dmat4& matrix)
{
    for (glm::length_t column = 0; column < 4; ++column)
        for (glm::length_t row = 0; row < 4; ++row)
            target[static_cast<std::size_t>(column * 4 + row)] = matrix[column][row];
}

std::int32_t validTexCoord(const int64_t value, TilesetConvertStats* stats)
{
    if (value < 0 || value > 1) {
        if (stats)
            ++stats->skippedTextureCoordinates;
        return -1;
    }
    return static_cast<std::int32_t>(value);
}

template <typename Variant> bool accessorValid(const Variant& accessor)
{
    return std::visit(
        [](const auto& view) { return view.status() == AccessorViewStatus::Valid; },
        accessor);
}

template <typename Variant> bool accessorValidForCount(const Variant& accessor, int64_t count)
{
    return std::visit(
        [count](const auto& view) {
            return view.status() == AccessorViewStatus::Valid && view.size() == count;
        },
        accessor);
}

struct InstanceMatrices {
    std::vector<glm::dmat4> matrices;
    bool valid = true;
};

InstanceMatrices instanceMatrices(const Model& model, const Node& node,
                                  std::uint32_t maximum)
{
    InstanceMatrices result;
    const auto* extension = node.getExtension<ExtensionExtMeshGpuInstancing>();
    if (!extension) {
        result.matrices.push_back(glm::dmat4(1.0));
        return result;
    }
    const auto translationIt = extension->attributes.find("TRANSLATION");
    const auto rotationIt = extension->attributes.find("ROTATION");
    const auto scaleIt = extension->attributes.find("SCALE");

    std::optional<AccessorView<AccessorTypes::VEC3<float>>> translations;
    if (translationIt != extension->attributes.end()) {
        translations.emplace(model, translationIt->second);
        if (translations->status() != AccessorViewStatus::Valid)
            translations.reset();
    }
    std::optional<AccessorView<AccessorTypes::VEC3<float>>> scales;
    if (scaleIt != extension->attributes.end()) {
        scales.emplace(model, scaleIt->second);
        if (scales->status() != AccessorViewStatus::Valid)
            scales.reset();
    }
    std::optional<QuaternionAccessorType> rotations;
    if (rotationIt != extension->attributes.end()) {
        QuaternionAccessorType view = getQuaternionAccessorView(model, rotationIt->second);
        const bool valid = std::visit(
            [](const auto& accessor) {
                return accessor.status() == AccessorViewStatus::Valid;
            },
            view);
        if (valid)
            rotations.emplace(std::move(view));
    }

    int64_t count = 1;
    if (translations)
        count = std::max<int64_t>(count, translations->size());
    if (scales)
        count = std::max<int64_t>(count, scales->size());
    if (rotations) {
        const int64_t rotationCount = std::visit(
            [](const auto& accessor) { return static_cast<int64_t>(accessor.size()); },
            *rotations);
        count = std::max(count, rotationCount);
    }
    if (count <= 0)
        count = 1;
    if (count > static_cast<int64_t>(maximum)) {
        result.valid = false;
        result.matrices.assign(static_cast<std::size_t>(count), glm::dmat4(1.0));
        return result;
    }

    result.matrices.reserve(static_cast<std::size_t>(count));
    for (int64_t index = 0; index < count; ++index) {
        glm::dvec3 translation(0.0);
        if (translations && index < translations->size())
            translation = glm::dvec3((*translations)[index].value[0],
                                     (*translations)[index].value[1],
                                     (*translations)[index].value[2]);
        glm::dvec3 scale(1.0);
        if (scales && index < scales->size())
            scale = glm::dvec3((*scales)[index].value[0], (*scales)[index].value[1],
                               (*scales)[index].value[2]);
        glm::dquat rotation(1.0, 0.0, 0.0, 0.0);
        if (rotations) {
            const std::optional<glm::dquat> value = std::visit(
                QuaternionFromAccessor { index }, *rotations);
            if (value)
                rotation = *value;
        }
        result.matrices.push_back(glm::translate(glm::dmat4(1.0), translation) *
                                  glm::mat4_cast(rotation) *
                                  glm::scale(glm::dmat4(1.0), scale));
    }
    return result;
}

class Converter {
  public:
    Converter(const Model& model, const glm::dmat4& contentToLocal,
              const TilesetConvertOptions& options, TilesetConvertStats* stats)
        : m_model(model), m_contentToLocal(contentToLocal), m_options(options),
          m_stats(stats)
    {
    }

    std::shared_ptr<View3D::ModelMeshDto> run()
    {
        if (m_model.meshes.empty()) {
            if (m_stats)
                m_stats->warnings.append(QStringLiteral("瓦片内容不含 mesh"));
            return {};
        }
        convertTextures();

        m_mesh.nodes.reserve(m_model.nodes.size() + 1u);
        View3D::ModelNodeDto rootNode;
        rootNode.name = QStringLiteral("3D Tiles 瓦片");
        rootNode.sourceNodeIndex = -1;
        assignTransform(rootNode.localTransform, m_contentToLocal);
        assignTransform(rootNode.worldTransform, m_contentToLocal);
        m_mesh.nodes.push_back(std::move(rootNode));
        m_mesh.rootNodes.push_back(0u);

        std::vector<int32_t> roots;
        const int32_t sceneIndex =
            m_model.scene >= 0 ? m_model.scene : 0;
        if (!m_model.scenes.empty() && sceneIndex >= 0 &&
            static_cast<std::size_t>(sceneIndex) < m_model.scenes.size()) {
            roots = m_model.scenes[static_cast<std::size_t>(sceneIndex)].nodes;
        }
        if (roots.empty() && !m_model.nodes.empty()) {
            std::vector<bool> isChild(m_model.nodes.size(), false);
            for (const Node& node : m_model.nodes) {
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

        m_activeNodes.assign(m_model.nodes.size(), false);
        for (const int32_t root : roots)
            appendNode(root, 0, m_contentToLocal, 0);

        // 有些 glTF 只有 meshes 没有 nodes（或场景为空），按 mesh 逐个挂到根下。
        if (m_mesh.primitives.empty() && m_model.nodes.empty()) {
            for (std::size_t meshIndex = 0; meshIndex < m_model.meshes.size(); ++meshIndex) {
                View3D::ModelNodeDto node;
                node.name = QStringLiteral("Mesh %1").arg(meshIndex);
                node.sourceNodeIndex = -1;
                node.meshIndex = static_cast<std::int32_t>(meshIndex);
                node.parentIndex = 0;
                assignTransform(node.localTransform, glm::dmat4(1.0));
                assignTransform(node.worldTransform, m_contentToLocal);
                const std::uint32_t index = static_cast<std::uint32_t>(m_mesh.nodes.size());
                m_mesh.nodes.push_back(std::move(node));
                m_mesh.nodes[0].children.push_back(index);
                appendMesh(static_cast<int32_t>(meshIndex), index);
            }
        }

        if (m_mesh.vertices.empty() || m_mesh.indices.empty() || m_mesh.primitives.empty()) {
            if (m_stats)
                m_stats->warnings.append(QStringLiteral("瓦片内容没有可渲染几何"));
            return {};
        }

        if (m_options.buildRayIndex) {
            m_mesh.rebuildRayIndex(m_options.rayIndexLeafSize);
        }
        if (m_stats) {
            m_stats->vertices = m_mesh.vertices.size();
            m_stats->indices = m_mesh.indices.size();
            m_stats->primitives = m_mesh.primitives.size();
            m_stats->nodes = m_mesh.nodes.size();
        }
        return std::make_shared<View3D::ModelMeshDto>(std::move(m_mesh));
    }

  private:
    void convertTextures()
    {
        m_textureRemap.assign(m_model.textures.size(), -1);
        m_imageTextures.assign(m_model.images.size(), -1);
        for (std::size_t textureIndex = 0; textureIndex < m_model.textures.size();
             ++textureIndex) {
            const Texture& texture = m_model.textures[textureIndex];
            if (texture.source < 0 ||
                static_cast<std::size_t>(texture.source) >= m_model.images.size())
                continue;
            const std::int32_t imageIndex = texture.source;
            if (m_imageTextures[static_cast<std::size_t>(imageIndex)] < 0) {
                const Image& image = m_model.images[static_cast<std::size_t>(imageIndex)];
                if (!image.pAsset)
                    continue;
                const CesiumImage::ImageAsset& asset = *image.pAsset;
                if (asset.compressedPixelFormat != CesiumImage::GpuCompressedPixelFormat::NONE ||
                    asset.bytesPerChannel != 1 || asset.channels < 1 || asset.channels > 4) {
                    if (m_stats)
                        ++m_stats->skippedCompressedTextures;
                    continue;
                }
                if (asset.width <= 0 || asset.height <= 0)
                    continue;
                const std::uint64_t pixelCount =
                    static_cast<std::uint64_t>(asset.width) *
                    static_cast<std::uint64_t>(asset.height);
                if (pixelCount == 0 || pixelCount > m_options.maxTexturePixels) {
                    if (m_stats)
                        ++m_stats->skippedCompressedTextures;
                    continue;
                }
                std::size_t offset = 0;
                std::size_t available = asset.pixelData.size();
                if (!asset.mipPositions.empty()) {
                    offset = asset.mipPositions.front().byteOffset;
                    available = asset.mipPositions.front().byteSize;
                }
                const std::size_t channels = static_cast<std::size_t>(asset.channels);
                if (offset > asset.pixelData.size() || available > asset.pixelData.size() - offset ||
                    available < pixelCount * channels)
                    continue;

                View3D::ModelTextureDto dto;
                dto.width = asset.width;
                dto.height = asset.height;
                if (texture.sampler >= 0 &&
                    static_cast<std::size_t>(texture.sampler) < m_model.samplers.size()) {
                    const Sampler& sampler = m_model.samplers[static_cast<std::size_t>(texture.sampler)];
                    dto.magFilter = sampler.magFilter.value_or(9729);
                    dto.minFilter = sampler.minFilter.value_or(9987);
                    dto.wrapS = sampler.wrapS;
                    dto.wrapT = sampler.wrapT;
                }
                dto.rgba8.resize(static_cast<std::size_t>(pixelCount) * 4u);
                const std::byte* source = asset.pixelData.data() + offset;
                for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(pixelCount);
                     ++pixel) {
                    const std::byte* input = source + pixel * channels;
                    std::byte* output = dto.rgba8.data() + pixel * 4u;
                    if (channels == 1u || channels == 2u) {
                        output[0] = input[0];
                        output[1] = input[0];
                        output[2] = input[0];
                        output[3] = channels == 2u ? input[1] : std::byte { 255 };
                    } else {
                        output[0] = input[0];
                        output[1] = input[1];
                        output[2] = input[2];
                        output[3] = channels == 4u ? input[3] : std::byte { 255 };
                    }
                }
                if (m_stats) {
                    ++m_stats->textures;
                    m_stats->textureBytes += dto.rgba8.size();
                }
                m_imageTextures[static_cast<std::size_t>(imageIndex)] =
                    static_cast<std::int32_t>(m_mesh.textures.size());
                m_mesh.textures.push_back(std::move(dto));
            }
            m_textureRemap[textureIndex] =
                m_imageTextures[static_cast<std::size_t>(imageIndex)];
        }
    }

    std::int32_t remapTexture(int32_t textureIndex, int64_t texCoord,
                              std::int32_t* outputTexCoord)
    {
        if (textureIndex < 0)
            return -1;
        const std::int32_t supported = validTexCoord(texCoord, m_stats);
        if (supported < 0)
            return -1;
        if (static_cast<std::size_t>(textureIndex) >= m_textureRemap.size() ||
            m_textureRemap[static_cast<std::size_t>(textureIndex)] < 0)
            return -1;
        if (outputTexCoord)
            *outputTexCoord = supported;
        return m_textureRemap[static_cast<std::size_t>(textureIndex)];
    }

    void appendNode(int32_t nodeIndex, int32_t parentDto, const glm::dmat4& parentWorld,
                    int depth)
    {
        if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= m_model.nodes.size() ||
            depth > 64) {
            if (m_stats)
                ++m_stats->invalidNodes;
            return;
        }
        std::vector<bool>& active = m_activeNodes;
        if (active[static_cast<std::size_t>(nodeIndex)]) {
            if (m_stats)
                ++m_stats->invalidNodes;
            return;
        }
        const Node& node = m_model.nodes[static_cast<std::size_t>(nodeIndex)];
        const std::optional<glm::dmat4> localTransform =
            CesiumGltfContent::GltfUtilities::getNodeTransform(node);
        if (!localTransform) {
            if (m_stats)
                ++m_stats->invalidNodes;
            return;
        }
        active[static_cast<std::size_t>(nodeIndex)] = true;

        const glm::dmat4 world = parentWorld * *localTransform;
        View3D::ModelNodeDto dtoNode;
        dtoNode.name = node.name.empty()
            ? QStringLiteral("节点 %1").arg(nodeIndex)
            : QString::fromStdString(node.name);
        dtoNode.sourceNodeIndex = nodeIndex;
        dtoNode.meshIndex = node.mesh;
        dtoNode.parentIndex = parentDto;
        assignTransform(dtoNode.localTransform, *localTransform);
        assignTransform(dtoNode.worldTransform, world);
        const std::uint32_t dtoIndex = static_cast<std::uint32_t>(m_mesh.nodes.size());
        m_mesh.nodes.push_back(std::move(dtoNode));
        if (parentDto >= 0 &&
            static_cast<std::size_t>(parentDto) < m_mesh.nodes.size()) {
            m_mesh.nodes[static_cast<std::size_t>(parentDto)].children.push_back(dtoIndex);
        } else {
            m_mesh.rootNodes.push_back(dtoIndex);
        }

        if (node.mesh >= 0) {
            if (static_cast<std::size_t>(node.mesh) >= m_model.meshes.size()) {
                if (m_stats)
                    ++m_stats->invalidNodes;
            } else if (node.getExtension<ExtensionExtMeshGpuInstancing>()) {
                appendInstancedMesh(node, node.mesh, dtoIndex, world);
            } else {
                appendMesh(node.mesh, dtoIndex);
            }
        }
        for (const int32_t child : node.children)
            appendNode(child, static_cast<int32_t>(dtoIndex), world, depth + 1);
        active[static_cast<std::size_t>(nodeIndex)] = false;
    }

    void appendInstancedMesh(const Node& node, int32_t meshIndex, std::uint32_t parentDto,
                             const glm::dmat4& parentWorld)
    {
        const InstanceMatrices instances =
            instanceMatrices(m_model, node, m_options.maxInstancesPerNode);
        if (!instances.valid) {
            if (m_stats) {
                m_stats->skippedInstances += instances.matrices.size();
                m_stats->warnings.append(
                    QStringLiteral("实例数超过上限 %1，已跳过该节点")
                        .arg(m_options.maxInstancesPerNode));
            }
            return;
        }
        for (std::size_t index = 0; index < instances.matrices.size(); ++index) {
            const glm::dmat4& instance = instances.matrices[index];
            View3D::ModelNodeDto instanceNode;
            instanceNode.name = QStringLiteral("实例 %1").arg(index);
            instanceNode.sourceNodeIndex = -1;
            instanceNode.parentIndex = static_cast<std::int32_t>(parentDto);
            assignTransform(instanceNode.localTransform, instance);
            assignTransform(instanceNode.worldTransform, parentWorld * instance);
            const std::uint32_t dtoIndex =
                static_cast<std::uint32_t>(m_mesh.nodes.size());
            m_mesh.nodes.push_back(std::move(instanceNode));
            m_mesh.nodes[static_cast<std::size_t>(parentDto)].children.push_back(dtoIndex);
            appendMesh(meshIndex, dtoIndex);
            if (m_stats)
                ++m_stats->instances;
        }
    }

    void appendMesh(int32_t meshIndex, std::uint32_t dtoNode)
    {
        if (meshIndex < 0 ||
            static_cast<std::size_t>(meshIndex) >= m_model.meshes.size() ||
            dtoNode >= m_mesh.nodes.size())
            return;
        const Mesh& mesh = m_model.meshes[static_cast<std::size_t>(meshIndex)];
        const std::uint32_t firstPrimitive =
            static_cast<std::uint32_t>(m_mesh.primitives.size());
        for (const MeshPrimitive& primitive : mesh.primitives)
            appendPrimitive(primitive);
        View3D::ModelNodeDto& node = m_mesh.nodes[dtoNode];
        node.firstPrimitive = firstPrimitive;
        node.primitiveCount =
            static_cast<std::uint32_t>(m_mesh.primitives.size()) - firstPrimitive;
    }

    void appendPrimitive(const MeshPrimitive& primitive)
    {
        const bool points = primitive.mode == MeshPrimitive::Mode::POINTS;
        const bool triangles = primitive.mode == MeshPrimitive::Mode::TRIANGLES ||
            primitive.mode == MeshPrimitive::Mode::TRIANGLE_STRIP ||
            primitive.mode == MeshPrimitive::Mode::TRIANGLE_FAN;
        if (!points && !triangles) {
            if (m_stats)
                ++m_stats->skippedPrimitives;
            return;
        }
        const auto positions = getPositionAccessorView(m_model, primitive);
        if (!accessorValid(positions)) {
            if (m_stats)
                ++m_stats->skippedPrimitives;
            return;
        }
        const int64_t vertexCount = std::visit(CountFromAccessor {}, positions);
        if (vertexCount <= 0) {
            if (m_stats)
                ++m_stats->skippedPrimitives;
            return;
        }
        if (m_mesh.vertices.size() + static_cast<std::uint64_t>(vertexCount) >
            m_options.maxVerticesPerTile) {
            if (m_stats) {
                ++m_stats->overBudgetTiles;
                m_stats->warnings.append(
                    QStringLiteral("单块瓦片顶点数超过上限 %1，已跳过该基元")
                        .arg(m_options.maxVerticesPerTile));
            }
            return;
        }
        const auto indices = getIndexAccessorView(m_model, primitive);
        const int64_t sourceIndexCount = std::visit(NumIndicesFromAccessor {}, indices);
        if (triangles && sourceIndexCount > 0 && sourceIndexCount < 3) {
            if (m_stats)
                ++m_stats->skippedPrimitives;
            return;
        }

        const auto normals = getNormalAccessorView(m_model, primitive);
        const auto texCoords = getTexCoordAccessorView(m_model, primitive, 0);
        const auto texCoords1 = getTexCoordAccessorView(m_model, primitive, 1);
        const auto colors = getColorAccessorView(m_model, primitive, 0);
        std::optional<AccessorView<AccessorTypes::VEC4<float>>> tangents;
        const auto tangentAttribute = primitive.attributes.find("TANGENT");
        if (tangentAttribute != primitive.attributes.end()) {
            tangents.emplace(m_model, tangentAttribute->second);
            if (tangents->status() != AccessorViewStatus::Valid ||
                tangents->size() != vertexCount)
                tangents.reset();
        }

        const std::uint32_t firstVertex =
            static_cast<std::uint32_t>(m_mesh.vertices.size());
        const bool hasNormals = accessorValidForCount(normals, vertexCount);
        const bool hasTexCoords = accessorValidForCount(texCoords, vertexCount);
        const bool hasTexCoords1 = accessorValidForCount(texCoords1, vertexCount);
        const bool hasColors = accessorValidForCount(colors, vertexCount);
        for (int64_t index = 0; index < vertexCount; ++index) {
            const auto position = std::visit(PositionFromAccessor { index }, positions);
            if (!position) {
                if (m_stats)
                    ++m_stats->skippedPrimitives;
                m_mesh.vertices.resize(firstVertex);
                return;
            }
            View3D::MeshVertexDto vertex;
            vertex.x = position->x;
            vertex.y = position->y;
            vertex.z = position->z;
            if (hasNormals) {
                const auto normal = std::visit(NormalFromAccessor { index }, normals);
                if (normal) {
                    vertex.nx = normal->x;
                    vertex.ny = normal->y;
                    vertex.nz = normal->z;
                    vertex.hasNormal = true;
                }
            }
            if (hasTexCoords) {
                const auto texCoord = std::visit(TexCoordFromAccessor { index }, texCoords);
                if (texCoord) {
                    vertex.u = texCoord->x;
                    vertex.v = texCoord->y;
                    vertex.hasTexCoord = true;
                }
            }
            if (hasTexCoords1) {
                const auto texCoord1 =
                    std::visit(TexCoordFromAccessor { index }, texCoords1);
                if (texCoord1) {
                    vertex.u1 = texCoord1->x;
                    vertex.v1 = texCoord1->y;
                    vertex.hasTexCoord1 = true;
                }
            }
            if (hasColors) {
                const auto color = std::visit(ColorFromAccessor { index }, colors);
                if (color) {
                    vertex.colorR = static_cast<std::uint8_t>(
                        std::clamp(color->x, 0.0, 1.0) * 255.0);
                    vertex.colorG = static_cast<std::uint8_t>(
                        std::clamp(color->y, 0.0, 1.0) * 255.0);
                    vertex.colorB = static_cast<std::uint8_t>(
                        std::clamp(color->z, 0.0, 1.0) * 255.0);
                    vertex.colorA = static_cast<std::uint8_t>(
                        std::clamp(color->w, 0.0, 1.0) * 255.0);
                    vertex.hasColor = true;
                }
            }
            if (tangents) {
                const auto tangent = (*tangents)[index];
                const double length = std::sqrt(
                    static_cast<double>(tangent.value[0]) * tangent.value[0] +
                    static_cast<double>(tangent.value[1]) * tangent.value[1] +
                    static_cast<double>(tangent.value[2]) * tangent.value[2]);
                if (std::isfinite(length) && length > 1e-12) {
                    vertex.tx = tangent.value[0] / length;
                    vertex.ty = tangent.value[1] / length;
                    vertex.tz = tangent.value[2] / length;
                    vertex.tw = tangent.value[3] < 0.0f ? -1.0 : 1.0;
                    vertex.hasTangent = true;
                }
            }
            m_mesh.vertices.push_back(vertex);
        }
        if (!hasNormals) {
            if (m_stats)
                m_stats->missingNormals += static_cast<std::uint64_t>(vertexCount);
        }

        const std::uint32_t firstIndex =
            static_cast<std::uint32_t>(m_mesh.indices.size());
        std::vector<std::uint32_t> sourceIndices;
        if (sourceIndexCount > 0) {
            sourceIndices.reserve(static_cast<std::size_t>(sourceIndexCount));
            for (int64_t index = 0; index < sourceIndexCount; ++index) {
                const std::int64_t value =
                    std::visit(IndexFromAccessor { index }, indices);
                if (value < 0 || value >= vertexCount) {
                    if (m_stats)
                        ++m_stats->skippedPrimitives;
                    m_mesh.vertices.resize(firstVertex);
                    m_mesh.indices.resize(firstIndex);
                    return;
                }
                sourceIndices.push_back(static_cast<std::uint32_t>(value));
            }
        } else {
            sourceIndices.reserve(static_cast<std::size_t>(vertexCount));
            for (int64_t index = 0; index < vertexCount; ++index)
                sourceIndices.push_back(static_cast<std::uint32_t>(index));
        }

        const auto emitTriangle = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            m_mesh.indices.push_back(firstVertex + a);
            m_mesh.indices.push_back(firstVertex + b);
            m_mesh.indices.push_back(firstVertex + c);
        };
        if (points) {
            for (const std::uint32_t index : sourceIndices)
                m_mesh.indices.push_back(firstVertex + index);
        } else if (primitive.mode == MeshPrimitive::Mode::TRIANGLES) {
            const std::size_t count = sourceIndices.size() - sourceIndices.size() % 3u;
            for (std::size_t index = 0; index < count; index += 3u)
                emitTriangle(sourceIndices[index], sourceIndices[index + 1u],
                             sourceIndices[index + 2u]);
        } else if (primitive.mode == MeshPrimitive::Mode::TRIANGLE_STRIP) {
            for (std::size_t index = 0; index + 2u < sourceIndices.size(); ++index) {
                if ((index & 1u) == 0u)
                    emitTriangle(sourceIndices[index], sourceIndices[index + 1u],
                                 sourceIndices[index + 2u]);
                else
                    emitTriangle(sourceIndices[index + 1u], sourceIndices[index],
                                 sourceIndices[index + 2u]);
            }
        } else if (primitive.mode == MeshPrimitive::Mode::TRIANGLE_FAN) {
            for (std::size_t index = 1; index + 1u < sourceIndices.size(); ++index)
                emitTriangle(sourceIndices[0], sourceIndices[index], sourceIndices[index + 1u]);
        }

        if (m_mesh.indices.size() == firstIndex) {
            m_mesh.vertices.resize(firstVertex);
            if (m_stats)
                ++m_stats->skippedPrimitives;
            return;
        }

        if (!hasNormals && triangles && !points)
            generateNormals(firstVertex, firstIndex);

        View3D::ModelPrimitiveDto dto;
        dto.firstVertex = firstVertex;
        dto.vertexCount = static_cast<std::uint32_t>(vertexCount);
        dto.firstIndex = firstIndex;
        dto.indexCount =
            static_cast<std::uint32_t>(m_mesh.indices.size()) - firstIndex;
        dto.topology = points ? View3D::ModelPrimitiveTopology::Points
                              : View3D::ModelPrimitiveTopology::Triangles;
        applyMaterial(primitive, &dto, tangents.has_value());
        if (m_stats) {
            if (points)
                m_stats->points += dto.indexCount;
            else
                m_stats->triangles += dto.indexCount / 3u;
        }
        m_mesh.primitives.push_back(std::move(dto));
    }

    void generateNormals(std::uint32_t firstVertex, std::uint32_t firstIndex)
    {
        const std::uint32_t end = static_cast<std::uint32_t>(m_mesh.vertices.size());
        for (std::uint32_t index = firstVertex; index < end; ++index) {
            m_mesh.vertices[index].nx = 0.0;
            m_mesh.vertices[index].ny = 0.0;
            m_mesh.vertices[index].nz = 0.0;
            m_mesh.vertices[index].hasNormal = false;
        }
        const std::uint32_t indexEnd = static_cast<std::uint32_t>(m_mesh.indices.size());
        for (std::uint32_t index = firstIndex; index + 2u < indexEnd; index += 3u) {
            const std::uint32_t ia = m_mesh.indices[index];
            const std::uint32_t ib = m_mesh.indices[index + 1u];
            const std::uint32_t ic = m_mesh.indices[index + 2u];
            if (ia < firstVertex || ib < firstVertex || ic < firstVertex || ia >= end ||
                ib >= end || ic >= end)
                continue;
            const View3D::MeshVertexDto& a = m_mesh.vertices[ia];
            const View3D::MeshVertexDto& b = m_mesh.vertices[ib];
            const View3D::MeshVertexDto& c = m_mesh.vertices[ic];
            const double e1x = b.x - a.x;
            const double e1y = b.y - a.y;
            const double e1z = b.z - a.z;
            const double e2x = c.x - a.x;
            const double e2y = c.y - a.y;
            const double e2z = c.z - a.z;
            const double nx = e1y * e2z - e1z * e2y;
            const double ny = e1z * e2x - e1x * e2z;
            const double nz = e1x * e2y - e1y * e2x;
            for (const std::uint32_t target : { ia, ib, ic }) {
                m_mesh.vertices[target].nx += nx;
                m_mesh.vertices[target].ny += ny;
                m_mesh.vertices[target].nz += nz;
            }
        }
        for (std::uint32_t index = firstVertex; index < end; ++index) {
            View3D::MeshVertexDto& vertex = m_mesh.vertices[index];
            const double length =
                std::sqrt(vertex.nx * vertex.nx + vertex.ny * vertex.ny +
                          vertex.nz * vertex.nz);
            if (length > 1e-12) {
                vertex.nx /= length;
                vertex.ny /= length;
                vertex.nz /= length;
            } else {
                vertex.nx = 0.0;
                vertex.ny = 1.0;
                vertex.nz = 0.0;
            }
            vertex.hasNormal = true;
        }
    }

    void applyMaterial(const MeshPrimitive& primitive, View3D::ModelPrimitiveDto* dto,
                       bool hasTangents)
    {
        const Material* material = nullptr;
        if (primitive.material >= 0 &&
            static_cast<std::size_t>(primitive.material) < m_model.materials.size())
            material = &m_model.materials[static_cast<std::size_t>(primitive.material)];
        dto->materialIndex = primitive.material;

        double baseR = 1.0;
        double baseG = 1.0;
        double baseB = 1.0;
        double baseA = 1.0;
        double metallic = 1.0;
        double roughness = 1.0;
        if (material) {
            dto->alphaBlend = material->alphaMode == Material::AlphaMode::BLEND;
            dto->alphaMask = material->alphaMode == Material::AlphaMode::MASK;
            dto->alphaCutoff = clampNumber(material->alphaCutoff, 0.5, 0.0, 1.0);
            dto->doubleSided = material->doubleSided;
            if (material->pbrMetallicRoughness) {
                const MaterialPBRMetallicRoughness& pbr = *material->pbrMetallicRoughness;
                if (pbr.baseColorFactor.size() >= 4) {
                    baseR = clampNumber(pbr.baseColorFactor[0], 1.0, 0.0, 1.0);
                    baseG = clampNumber(pbr.baseColorFactor[1], 1.0, 0.0, 1.0);
                    baseB = clampNumber(pbr.baseColorFactor[2], 1.0, 0.0, 1.0);
                    baseA = clampNumber(pbr.baseColorFactor[3], 1.0, 0.0, 1.0);
                }
                metallic = clampNumber(pbr.metallicFactor, 1.0, 0.0, 1.0);
                roughness = clampNumber(pbr.roughnessFactor, 1.0, 0.0, 1.0);
                if (pbr.baseColorTexture) {
                    dto->baseColorTextureIndex =
                        remapTexture(pbr.baseColorTexture->index,
                                     pbr.baseColorTexture->texCoord,
                                     &dto->baseColorTexCoord);
                }
                if (pbr.metallicRoughnessTexture) {
                    dto->metallicRoughnessTextureIndex =
                        remapTexture(pbr.metallicRoughnessTexture->index,
                                     pbr.metallicRoughnessTexture->texCoord,
                                     &dto->metallicRoughnessTexCoord);
                }
            }
            if (material->normalTexture) {
                dto->normalTextureIndex =
                    remapTexture(material->normalTexture->index,
                                 material->normalTexture->texCoord,
                                 &dto->normalTexCoord);
                dto->normalScale =
                    clampNumber(material->normalTexture->scale, 1.0, 0.0, 10.0);
            }
            if (material->occlusionTexture) {
                dto->occlusionTextureIndex =
                    remapTexture(material->occlusionTexture->index,
                                 material->occlusionTexture->texCoord,
                                 &dto->occlusionTexCoord);
                dto->occlusionStrength =
                    clampNumber(material->occlusionTexture->strength, 1.0, 0.0, 1.0);
            }
            if (material->emissiveTexture) {
                dto->emissiveTextureIndex =
                    remapTexture(material->emissiveTexture->index,
                                 material->emissiveTexture->texCoord,
                                 &dto->emissiveTexCoord);
            }
            if (material->emissiveFactor.size() >= 3) {
                dto->emissiveR =
                    clampNumber(material->emissiveFactor[0], 0.0, 0.0, 1.0);
                dto->emissiveG =
                    clampNumber(material->emissiveFactor[1], 0.0, 0.0, 1.0);
                dto->emissiveB =
                    clampNumber(material->emissiveFactor[2], 0.0, 0.0, 1.0);
            }
        }

        dto->baseColorR = baseR;
        dto->baseColorG = baseG;
        dto->baseColorB = baseB;
        dto->baseColorA = baseA;
        dto->metallicFactor = metallic;
        dto->roughnessFactor = roughness;

        const bool forceUnlit =
            material && material->getExtension<ExtensionKhrMaterialsUnlit>() != nullptr;
        if (m_options.unlit || forceUnlit) {
            const bool textureFree = dto->baseColorTextureIndex < 0 &&
                dto->metallicRoughnessTextureIndex < 0 &&
                dto->normalTextureIndex < 0 && dto->occlusionTextureIndex < 0 &&
                dto->emissiveTextureIndex < 0;
            bool vertexColoured = false;
            const std::uint32_t vertexEnd =
                dto->firstVertex + dto->vertexCount;
            for (std::uint32_t index = dto->firstVertex;
                 index < vertexEnd && index < m_mesh.vertices.size(); ++index) {
                if (m_mesh.vertices[index].hasColor) {
                    vertexColoured = true;
                    break;
                }
            }
            if (textureFree && vertexColoured) {
                // Renderer 对"无贴图 + 逐点颜色"的基元走 HlmsUnlit::setUseColour，
                // 那里的基色是乘数：这里若像贴图路径一样置黑，点云会被乘成黑色。
                dto->metallicFactor = 0.0;
                dto->roughnessFactor = 1.0;
                dto->normalTextureIndex = -1;
                dto->normalMappingEnabled = false;
                dto->occlusionTextureIndex = -1;
                return;
            }
            const std::int32_t baseTexture = dto->baseColorTextureIndex;
            const std::int32_t baseTexCoord = dto->baseColorTexCoord;
            dto->emissiveTextureIndex = baseTexture;
            dto->emissiveTexCoord = baseTexCoord;
            dto->emissiveR = baseR;
            dto->emissiveG = baseG;
            dto->emissiveB = baseB;
            dto->baseColorR = 0.0;
            dto->baseColorG = 0.0;
            dto->baseColorB = 0.0;
            dto->metallicFactor = 0.0;
            dto->roughnessFactor = 1.0;
            dto->metallicRoughnessTextureIndex = -1;
            dto->normalTextureIndex = -1;
            dto->normalMappingEnabled = false;
            dto->occlusionTextureIndex = -1;
        } else if (dto->normalTextureIndex >= 0 && hasTangents) {
            dto->normalMappingEnabled = true;
            dto->tangentSource = View3D::ModelTangentSource::Explicit;
        }
    }

    const Model& m_model;
    const glm::dmat4& m_contentToLocal;
    const TilesetConvertOptions& m_options;
    TilesetConvertStats* m_stats = nullptr;
    View3D::ModelMeshDto m_mesh;
    std::vector<std::int32_t> m_textureRemap;
    std::vector<std::int32_t> m_imageTextures;
    std::vector<bool> m_activeNodes;
};

} // namespace

std::shared_ptr<View3D::ModelMeshDto> convertModelToMesh(
    const CesiumGltf::Model& model, const glm::dmat4& contentToLocal,
    const TilesetConvertOptions& options, TilesetConvertStats* stats)
{
    Converter converter(model, contentToLocal, options, stats);
    return converter.run();
}

} // namespace View3DTiles

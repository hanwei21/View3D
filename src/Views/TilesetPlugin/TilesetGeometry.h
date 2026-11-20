#pragma once

#include <ModelMesh.h>

#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>

#include <glm/mat4x4.hpp>

namespace CesiumGltf {
struct Model;
}

namespace View3DTiles {

struct TilesetConvertOptions {
    // CesiumJS 的 3D Tiles 默认 enableLighting=false：贴图按自发光显示，
    // 与倾斜摄影（影像已烘焙光照）的观感一致。VIEW3D_TILES_LIT=1 可退回 PBR。
    bool unlit = true;
    bool buildRayIndex = true;
    std::uint32_t rayIndexLeafSize = 8;
    // i3dm / EXT_mesh_gpu_instancing 在 View3D 的 DTO 里没有实例化通道，
    // 只能把实例展开成独立节点并复制几何。超过上限的节点直接跳过并告警。
    std::uint32_t maxInstancesPerNode = 2048;
    std::uint64_t maxVerticesPerTile = 4'000'000ull;
    std::uint64_t maxTexturePixels = 64ull * 1024ull * 1024ull;
};

struct TilesetConvertStats {
    std::uint64_t vertices = 0;
    std::uint64_t indices = 0;
    std::uint64_t primitives = 0;
    std::uint64_t nodes = 0;
    std::uint64_t instances = 0;
    std::uint64_t points = 0;
    std::uint64_t triangles = 0;
    std::uint64_t textures = 0;
    std::uint64_t textureBytes = 0;
    std::uint64_t missingNormals = 0;
    std::uint64_t skippedPrimitives = 0;
    std::uint64_t skippedCompressedTextures = 0;
    std::uint64_t skippedTextureCoordinates = 0;
    std::uint64_t skippedInstances = 0;
    std::uint64_t invalidNodes = 0;
    std::uint64_t overBudgetTiles = 0;
    QStringList warnings;

    TilesetConvertStats& operator+=(const TilesetConvertStats& other)
    {
        vertices += other.vertices;
        indices += other.indices;
        primitives += other.primitives;
        nodes += other.nodes;
        instances += other.instances;
        points += other.points;
        triangles += other.triangles;
        textures += other.textures;
        textureBytes += other.textureBytes;
        missingNormals += other.missingNormals;
        skippedPrimitives += other.skippedPrimitives;
        skippedCompressedTextures += other.skippedCompressedTextures;
        skippedTextureCoordinates += other.skippedTextureCoordinates;
        skippedInstances += other.skippedInstances;
        invalidNodes += other.invalidNodes;
        overBudgetTiles += other.overBudgetTiles;
        warnings.append(other.warnings);
        return *this;
    }
};

// contentToLocal = ENU 局部化矩阵 × 瓦片 ECEF 变换 × RTC × glTF up-axis。
// 生成的 DTO 顶点保持 glTF/节点局部坐标，变换放在节点层级里，与 Renderer 一致。
std::shared_ptr<View3D::ModelMeshDto> convertModelToMesh(
    const CesiumGltf::Model& model, const glm::dmat4& contentToLocal,
    const TilesetConvertOptions& options, TilesetConvertStats* stats);

} // namespace View3DTiles

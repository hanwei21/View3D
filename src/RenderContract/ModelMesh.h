#pragma once

#include <QtCore/QString>
#include <QtCore/QVariantMap>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace View3D {

struct MeshVertexDto {
    double x { 0.0 };
    double y { 0.0 };
    double z { 0.0 };
    double nx { 0.0 };
    double ny { 0.0 };
    double nz { 1.0 };
    double u { 0.0 };
    double v { 0.0 };
    bool hasNormal { false };
    bool hasTexCoord { false };
    double u1 { 0.0 };
    double v1 { 0.0 };
    double tx { 1.0 };
    double ty { 0.0 };
    double tz { 0.0 };
    double tw { 1.0 };
    bool hasTexCoord1 { false };
    bool hasTangent { false };
    std::uint8_t colorR { 255 };
    std::uint8_t colorG { 255 };
    std::uint8_t colorB { 255 };
    std::uint8_t colorA { 255 };
    bool hasColor { false };
};

enum class ModelPrimitiveTopology { Triangles, Points };
enum class ModelTangentSource { None, Explicit, Generated };

struct ModelPrimitiveDto {
    std::uint32_t firstVertex { 0 };
    std::uint32_t vertexCount { 0 };
    std::uint32_t firstIndex { 0 };
    std::uint32_t indexCount { 0 };
    std::int32_t materialIndex { -1 };
    std::int32_t baseColorTextureIndex { -1 };
    std::int32_t baseColorTexCoord { 0 };
    double baseColorR { 1.0 };
    double baseColorG { 1.0 };
    double baseColorB { 1.0 };
    double baseColorA { 1.0 };
    bool alphaBlend { false };
    bool alphaMask { false };
    double alphaCutoff { 0.5 };
    bool doubleSided { false };
    double metallicFactor { 1.0 };
    double roughnessFactor { 1.0 };
    std::int32_t metallicRoughnessTextureIndex { -1 };
    std::int32_t metallicRoughnessTexCoord { 0 };
    std::int32_t normalTextureIndex { -1 };
    std::int32_t normalTexCoord { 0 };
    double normalScale { 1.0 };
    std::int32_t occlusionTextureIndex { -1 };
    std::int32_t occlusionTexCoord { 0 };
    double occlusionStrength { 1.0 };
    std::int32_t emissiveTextureIndex { -1 };
    std::int32_t emissiveTexCoord { 0 };
    double emissiveR { 0.0 };
    double emissiveG { 0.0 };
    double emissiveB { 0.0 };
    ModelTangentSource tangentSource { ModelTangentSource::None };
    bool normalMappingEnabled { false };
    ModelPrimitiveTopology topology { ModelPrimitiveTopology::Triangles };
    std::int32_t featureTableIndex { -1 };
    std::int32_t featureIdSetIndex { -1 };
    std::vector<std::int64_t> featureIds;
};

struct ModelFeaturePropertyDto {
    QString id;
    QString name;
    QString type;
    std::vector<QString> values;
};

struct ModelFeatureTableDto {
    QString name;
    std::int64_t featureCount { 0 };
    std::vector<ModelFeaturePropertyDto> properties;
};

struct ModelFeatureHitDto {
    std::uint32_t primitiveIndex { 0 };
    std::uint32_t triangleIndex { 0 };
    std::int32_t tableIndex { -1 };
    std::int64_t featureId { -1 };
    QVariantMap properties;
};

struct ModelRayHitDto {
    std::uint32_t nodeIndex { 0 };
    std::uint32_t primitiveIndex { 0 };
    std::uint32_t triangleIndex { 0 };
    double distance { 0.0 };
    std::array<double, 3> position {};
    std::array<std::array<double, 3>, 3> triangleVertices {};
    bool hasTriangleVertices { false };
    bool hasFeature { false };
    ModelFeatureHitDto feature;
};

struct ModelRayTriangleIndexDto {
    std::array<double, 3> minimum {};
    std::array<double, 3> maximum {};
    std::array<double, 3> centroid {};
    std::uint32_t nodeIndex { 0 };
    std::uint32_t primitiveIndex { 0 };
    std::uint32_t triangleIndex { 0 };
};

struct ModelRayNodeIndexDto {
    std::array<double, 3> minimum {};
    std::array<double, 3> maximum {};
    std::int32_t left { -1 };
    std::int32_t right { -1 };
    std::uint32_t firstTriangle { 0 };
    std::uint32_t triangleCount { 0 };
};

struct ModelRayQueryStatisticsDto {
    std::uint32_t boundsTested { 0 };
    std::uint32_t trianglesTested { 0 };
};

struct ModelTextureDto {
    std::int32_t width { 0 };
    std::int32_t height { 0 };
    std::int32_t magFilter { 9729 };
    std::int32_t minFilter { 9987 };
    std::int32_t wrapS { 10497 };
    std::int32_t wrapT { 10497 };
    std::vector<std::byte> rgba8;
};

struct ModelNodeDto {
    QString name;
    std::int32_t sourceNodeIndex { -1 };
    std::int32_t meshIndex { -1 };
    std::int32_t parentIndex { -1 };
    std::vector<std::uint32_t> children;
    std::uint32_t firstPrimitive { 0 };
    std::uint32_t primitiveCount { 0 };
    std::array<double, 16> localTransform {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0 };
    std::array<double, 16> worldTransform {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0 };
};

struct ModelMeshDto {
    std::vector<MeshVertexDto> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<ModelPrimitiveDto> primitives;
    std::vector<ModelTextureDto> textures;
    std::vector<ModelNodeDto> nodes;
    std::vector<std::uint32_t> rootNodes;
    std::vector<ModelFeatureTableDto> featureTables;
    std::vector<ModelRayTriangleIndexDto> rayTriangles;
    std::vector<ModelRayNodeIndexDto> rayNodes;
    std::uint32_t rayIndexLeafSize = 0;
    // 动态投影用的源地理坐标缓存：与 vertices 一一对应的 (经度, 纬度, 椭球高)
    // 三元组，geoCrs 非空时才有内容。切换显示参照系靠它把已导入图层原地重放，
    // 不必回头再读源文件；没有参照系的数据源（普通模型、点云）留空。
    QString geoCrs;
    std::vector<double> geoVertices;
    // 这批顶点是按哪个显示参照系摆出来的：重放时用它求法线的投影旋转量，
    // 位置本身一律按 geoVertices 重新算，不受旧参照系影响。
    QString geoDisplayCrs;
    // 球面模式的瓦片内容原点（ECEF 米）：顶点/节点变换相对它烘焙，渲染端按
    // contentOrigin − renderOrigin 摆放。只有球面烘焙路径赋值；平面模式下
    // 保持 false，摆放行为不变。
    bool hasContentOrigin { false };
    std::array<double, 3> contentOrigin {};

    QVariantMap featureProperties(std::int32_t tableIndex, std::int64_t featureId) const;
    bool featureHitForTriangle(std::uint32_t primitiveIndex,
                               std::uint32_t triangleIndex,
                               ModelFeatureHitDto* hit) const;
    std::vector<std::uint32_t> featureTriangles(std::uint32_t primitiveIndex,
                                                std::int64_t featureId) const;
    bool rayHit(const std::array<double, 3>& origin,
                const std::array<double, 3>& direction,
                ModelRayHitDto* hit) const;
    // 点云没有三角形可相交：在射线周围 radius 内找最近的顶点。
    // 仅用于视图枢轴/拾取提示，命中时 exactTriangle 仍为 false。
    bool rayHitPoints(const std::array<double, 3>& origin,
                      const std::array<double, 3>& direction, double radius,
                      ModelRayHitDto* hit) const;
    bool rebuildRayIndex(
        std::uint32_t leafSize = 8,
        const std::function<bool()>& cancellationRequested = {});
    bool rayHitIndexed(const std::array<double, 3>& origin,
                       const std::array<double, 3>& direction,
                       ModelRayHitDto* hit,
                       ModelRayQueryStatisticsDto* statistics = nullptr) const;
};

inline std::size_t modelMeshCpuBytes(const ModelMeshDto& mesh) {
    std::size_t bytes = mesh.vertices.size() * sizeof(MeshVertexDto) +
                        mesh.indices.size() * sizeof(std::uint32_t) +
                        mesh.primitives.size() * sizeof(ModelPrimitiveDto) +
                        mesh.nodes.size() * sizeof(ModelNodeDto) +
                        mesh.rayTriangles.size() * sizeof(ModelRayTriangleIndexDto) +
                        mesh.rayNodes.size() * sizeof(ModelRayNodeIndexDto);
    bytes += mesh.geoVertices.size() * sizeof(double);
    bytes += sizeof(mesh.contentOrigin) + sizeof(mesh.hasContentOrigin);
    for (const ModelTextureDto& texture : mesh.textures)
        bytes += texture.rgba8.size();
    return bytes;
}

} // namespace View3D

#include "ModelMesh.h"

#include <QtCore/QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace View3D {

QVariantMap ModelMeshDto::featureProperties(std::int32_t tableIndex,
                                            std::int64_t featureId) const
{
    QVariantMap result;
    if (tableIndex < 0 || static_cast<std::size_t>(tableIndex) >= featureTables.size())
        return result;
    const ModelFeatureTableDto& table = featureTables[static_cast<std::size_t>(tableIndex)];
    if (featureId < 0 || featureId >= table.featureCount) return result;
    for (const ModelFeaturePropertyDto& property : table.properties) {
        if (static_cast<std::size_t>(featureId) >= property.values.size()) continue;
        result.insert(property.name.isEmpty() ? property.id : property.name,
                      property.values[static_cast<std::size_t>(featureId)]);
    }
    return result;
}

bool ModelMeshDto::featureHitForTriangle(std::uint32_t primitiveIndex,
                                         std::uint32_t triangleIndex,
                                         ModelFeatureHitDto* hit) const
{
    if (hit) *hit = {};
    if (!hit || primitiveIndex >= primitives.size()) return false;
    const ModelPrimitiveDto& primitive = primitives[primitiveIndex];
    if (primitive.topology != ModelPrimitiveTopology::Triangles ||
        primitive.featureTableIndex < 0 || primitive.featureIds.empty() ||
        primitive.featureIds.size() != primitive.vertexCount ||
        primitive.firstIndex > indices.size() ||
        primitive.indexCount > indices.size() - primitive.firstIndex ||
        triangleIndex >= primitive.indexCount / 3) return false;

    std::int64_t triangleFeatureId = -1;
    for (std::uint32_t corner = 0; corner < 3; ++corner) {
        const std::uint32_t globalVertex =
            indices[primitive.firstIndex + triangleIndex * 3 + corner];
        if (globalVertex < primitive.firstVertex ||
            globalVertex >= primitive.firstVertex + primitive.vertexCount) return false;
        const std::int64_t featureId =
            primitive.featureIds[globalVertex - primitive.firstVertex];
        if (featureId < 0) return false;
        if (corner == 0) triangleFeatureId = featureId;
        else if (featureId != triangleFeatureId) return false;
    }
    if (static_cast<std::size_t>(primitive.featureTableIndex) >= featureTables.size() ||
        triangleFeatureId >= featureTables[primitive.featureTableIndex].featureCount) return false;

    hit->primitiveIndex = primitiveIndex;
    hit->triangleIndex = triangleIndex;
    hit->tableIndex = primitive.featureTableIndex;
    hit->featureId = triangleFeatureId;
    hit->properties = featureProperties(hit->tableIndex, hit->featureId);
    return true;
}

std::vector<std::uint32_t> ModelMeshDto::featureTriangles(
    std::uint32_t primitiveIndex, std::int64_t featureId) const
{
    std::vector<std::uint32_t> result;
    if (primitiveIndex >= primitives.size() || featureId < 0) return result;
    const ModelPrimitiveDto& primitive = primitives[primitiveIndex];
    if (primitive.topology != ModelPrimitiveTopology::Triangles) return result;
    const std::uint32_t triangleCount = primitive.indexCount / 3;
    for (std::uint32_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
        ModelFeatureHitDto hit;
        if (featureHitForTriangle(primitiveIndex, triangleIndex, &hit) &&
            hit.featureId == featureId) result.push_back(triangleIndex);
    }
    return result;
}

bool ModelMeshDto::rayHit(const std::array<double, 3>& origin,
                          const std::array<double, 3>& direction,
                          ModelRayHitDto* hit) const
{
    if (hit) *hit = {};
    if (!hit) return false;
    const auto subtract = [](const std::array<double, 3>& a,
                             const std::array<double, 3>& b) {
        return std::array<double, 3> {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    const auto cross = [](const std::array<double, 3>& a,
                          const std::array<double, 3>& b) {
        return std::array<double, 3> {
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
    };
    const auto dot = [](const std::array<double, 3>& a,
                        const std::array<double, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const double directionLengthSquared = dot(direction, direction);
    if (!std::isfinite(directionLengthSquared) || directionLengthSquared < 1e-18) return false;

    bool found = false;
    double nearest = std::numeric_limits<double>::max();
    for (std::uint32_t primitiveIndex = 0; primitiveIndex < primitives.size(); ++primitiveIndex) {
        const ModelPrimitiveDto& primitive = primitives[primitiveIndex];
        if (primitive.topology != ModelPrimitiveTopology::Triangles ||
            primitive.firstIndex > indices.size() ||
            primitive.indexCount > indices.size() - primitive.firstIndex) continue;
        std::uint32_t nodeIndex = 0;
        const ModelNodeDto* ownerNode = nullptr;
        for (std::uint32_t candidate = 0; candidate < nodes.size(); ++candidate) {
            const ModelNodeDto& node = nodes[candidate];
            if (primitiveIndex >= node.firstPrimitive &&
                primitiveIndex < node.firstPrimitive + node.primitiveCount) {
                nodeIndex = candidate;
                ownerNode = &node;
                break;
            }
        }
        const auto worldVertex = [&](std::uint32_t globalIndex,
                                     std::array<double, 3>* value) {
            if (!value || globalIndex >= vertices.size()) return false;
            const MeshVertexDto& vertex = vertices[globalIndex];
            *value = {vertex.x, vertex.y, vertex.z};
            if (!ownerNode) return true;
            const auto& m = ownerNode->worldTransform;
            *value = {
                m[0] * vertex.x + m[4] * vertex.y + m[8] * vertex.z + m[12],
                m[1] * vertex.x + m[5] * vertex.y + m[9] * vertex.z + m[13],
                m[2] * vertex.x + m[6] * vertex.y + m[10] * vertex.z + m[14]};
            return true;
        };
        const std::uint32_t triangleCount = primitive.indexCount / 3;
        for (std::uint32_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
            std::array<double, 3> a, b, c;
            if (!worldVertex(indices[primitive.firstIndex + triangleIndex * 3], &a) ||
                !worldVertex(indices[primitive.firstIndex + triangleIndex * 3 + 1], &b) ||
                !worldVertex(indices[primitive.firstIndex + triangleIndex * 3 + 2], &c)) continue;
            const std::array<double, 3> edge1 = subtract(b, a);
            const std::array<double, 3> edge2 = subtract(c, a);
            const std::array<double, 3> p = cross(direction, edge2);
            const double determinant = dot(edge1, p);
            if (qAbs(determinant) < 1e-12) continue;
            const double inverseDeterminant = 1.0 / determinant;
            const std::array<double, 3> fromA = subtract(origin, a);
            const double u = dot(fromA, p) * inverseDeterminant;
            if (u < 0.0 || u > 1.0) continue;
            const std::array<double, 3> q = cross(fromA, edge1);
            const double v = dot(direction, q) * inverseDeterminant;
            if (v < 0.0 || u + v > 1.0) continue;
            const double distance = dot(edge2, q) * inverseDeterminant;
            if (distance < 0.0 || distance >= nearest || !std::isfinite(distance)) continue;
            nearest = distance;
            found = true;
            hit->nodeIndex = nodeIndex;
            hit->primitiveIndex = primitiveIndex;
            hit->triangleIndex = triangleIndex;
            hit->distance = distance;
            hit->position = {
                origin[0] + direction[0] * distance,
                origin[1] + direction[1] * distance,
                origin[2] + direction[2] * distance};
            hit->triangleVertices = {a, b, c};
            hit->hasTriangleVertices = true;
            hit->hasFeature = featureHitForTriangle(primitiveIndex, triangleIndex, &hit->feature);
        }
    }
    return found;
}

bool ModelMeshDto::rayHitPoints(const std::array<double, 3>& origin,
                                const std::array<double, 3>& direction, double radius,
                                ModelRayHitDto* hit) const
{
    if (hit) *hit = {};
    if (!hit || !std::isfinite(radius) || radius <= 0.0) return false;
    const double directionLengthSquared = direction[0] * direction[0] +
        direction[1] * direction[1] + direction[2] * direction[2];
    if (!std::isfinite(directionLengthSquared) || directionLengthSquared < 1e-18) return false;
    const double inverseLength = 1.0 / std::sqrt(directionLengthSquared);
    const std::array<double, 3> unit {
        direction[0] * inverseLength, direction[1] * inverseLength,
        direction[2] * inverseLength};
    const double radiusSquared = radius * radius;

    bool found = false;
    double nearest = std::numeric_limits<double>::max();
    for (std::uint32_t primitiveIndex = 0; primitiveIndex < primitives.size(); ++primitiveIndex) {
        const ModelPrimitiveDto& primitive = primitives[primitiveIndex];
        if (primitive.topology != ModelPrimitiveTopology::Points) continue;
        std::uint32_t nodeIndex = 0;
        const ModelNodeDto* ownerNode = nullptr;
        for (std::uint32_t candidate = 0; candidate < nodes.size(); ++candidate) {
            const ModelNodeDto& node = nodes[candidate];
            if (primitiveIndex >= node.firstPrimitive &&
                primitiveIndex < node.firstPrimitive + node.primitiveCount) {
                nodeIndex = candidate;
                ownerNode = &node;
                break;
            }
        }
        const auto worldPoint = [&](std::uint32_t globalIndex,
                                    std::array<double, 3>* value) {
            if (!value || globalIndex >= vertices.size()) return false;
            const MeshVertexDto& vertex = vertices[globalIndex];
            *value = {vertex.x, vertex.y, vertex.z};
            if (ownerNode) {
                const auto& m = ownerNode->worldTransform;
                *value = {
                    m[0] * vertex.x + m[4] * vertex.y + m[8] * vertex.z + m[12],
                    m[1] * vertex.x + m[5] * vertex.y + m[9] * vertex.z + m[13],
                    m[2] * vertex.x + m[6] * vertex.y + m[10] * vertex.z + m[14]};
            }
            return true;
        };
        const bool indexed = primitive.indexCount > 0 &&
            primitive.firstIndex < indices.size() &&
            primitive.indexCount <= indices.size() - primitive.firstIndex;
        const std::uint32_t count = indexed ? primitive.indexCount : primitive.vertexCount;
        for (std::uint32_t offset = 0; offset < count; ++offset) {
            const std::uint32_t globalIndex = indexed
                ? indices[primitive.firstIndex + offset]
                : primitive.firstVertex + offset;
            std::array<double, 3> point;
            if (!worldPoint(globalIndex, &point)) continue;
            const std::array<double, 3> delta {
                point[0] - origin[0], point[1] - origin[1], point[2] - origin[2]};
            const double along = delta[0] * unit[0] + delta[1] * unit[1] +
                delta[2] * unit[2];
            if (along < 0.0 || along >= nearest) continue;
            const double offsetX = delta[0] - unit[0] * along;
            const double offsetY = delta[1] - unit[1] * along;
            const double offsetZ = delta[2] - unit[2] * along;
            if (offsetX * offsetX + offsetY * offsetY + offsetZ * offsetZ >
                radiusSquared)
                continue;
            nearest = along;
            found = true;
            hit->nodeIndex = nodeIndex;
            hit->primitiveIndex = primitiveIndex;
            hit->triangleIndex = 0;
            hit->distance = along;
            hit->position = point;
            hit->triangleVertices = {};
            hit->hasTriangleVertices = false;
            hit->hasFeature = false;
        }
    }
    return found;
}

bool ModelMeshDto::rebuildRayIndex(
    std::uint32_t requestedLeafSize,
    const std::function<bool()>& cancellationRequested)
{
    rayTriangles.clear();
    rayNodes.clear();
    rayIndexLeafSize = qBound<std::uint32_t>(2, requestedLeafSize, 64);
    if (cancellationRequested && cancellationRequested()) {
        rayIndexLeafSize = 0;
        return false;
    }
    const auto worldVertex = [this](std::uint32_t nodeIndex, std::uint32_t globalIndex,
                                    std::array<double, 3>* value) {
        if (!value || globalIndex >= vertices.size()) return false;
        const MeshVertexDto& vertex = vertices[globalIndex];
        *value = {vertex.x, vertex.y, vertex.z};
        if (nodeIndex >= nodes.size()) return true;
        const auto& m = nodes[nodeIndex].worldTransform;
        *value = {
            m[0] * vertex.x + m[4] * vertex.y + m[8] * vertex.z + m[12],
            m[1] * vertex.x + m[5] * vertex.y + m[9] * vertex.z + m[13],
            m[2] * vertex.x + m[6] * vertex.y + m[10] * vertex.z + m[14]};
        return true;
    };
    for (std::uint32_t primitiveIndex = 0; primitiveIndex < primitives.size(); ++primitiveIndex) {
        if (cancellationRequested && cancellationRequested()) {
            rayTriangles.clear();
            rayIndexLeafSize = 0;
            return false;
        }
        const ModelPrimitiveDto& primitive = primitives[primitiveIndex];
        if (primitive.topology != ModelPrimitiveTopology::Triangles ||
            primitive.firstIndex > indices.size() ||
            primitive.indexCount > indices.size() - primitive.firstIndex) continue;
        std::uint32_t nodeIndex = std::numeric_limits<std::uint32_t>::max();
        for (std::uint32_t candidate = 0; candidate < nodes.size(); ++candidate) {
            const ModelNodeDto& node = nodes[candidate];
            if (primitiveIndex >= node.firstPrimitive &&
                primitiveIndex < node.firstPrimitive + node.primitiveCount) {
                nodeIndex = candidate;
                break;
            }
        }
        for (std::uint32_t triangleIndex = 0; triangleIndex < primitive.indexCount / 3;
             ++triangleIndex) {
            if ((triangleIndex & 255u) == 0u && cancellationRequested &&
                cancellationRequested()) {
                rayTriangles.clear();
                rayIndexLeafSize = 0;
                return false;
            }
            std::array<double, 3> a, b, c;
            if (!worldVertex(nodeIndex, indices[primitive.firstIndex + triangleIndex * 3], &a) ||
                !worldVertex(nodeIndex, indices[primitive.firstIndex + triangleIndex * 3 + 1], &b) ||
                !worldVertex(nodeIndex, indices[primitive.firstIndex + triangleIndex * 3 + 2], &c))
                continue;
            ModelRayTriangleIndexDto triangle;
            triangle.nodeIndex = nodeIndex == std::numeric_limits<std::uint32_t>::max()
                ? 0 : nodeIndex;
            triangle.primitiveIndex = primitiveIndex;
            triangle.triangleIndex = triangleIndex;
            for (int axis = 0; axis < 3; ++axis) {
                triangle.minimum[axis] = qMin(a[axis], qMin(b[axis], c[axis]));
                triangle.maximum[axis] = qMax(a[axis], qMax(b[axis], c[axis]));
                triangle.centroid[axis] = (a[axis] + b[axis] + c[axis]) / 3.0;
            }
            rayTriangles.push_back(triangle);
        }
    }
    if (rayTriangles.empty()) return true;
    const std::uint32_t leafSize = rayIndexLeafSize;
    std::function<std::int32_t(std::uint32_t, std::uint32_t)> build =
        [&](std::uint32_t begin, std::uint32_t end) -> std::int32_t {
            if (cancellationRequested && cancellationRequested()) return -1;
            ModelRayNodeIndexDto node;
            node.minimum.fill(std::numeric_limits<double>::max());
            node.maximum.fill(std::numeric_limits<double>::lowest());
            std::array<double, 3> centroidMinimum;
            std::array<double, 3> centroidMaximum;
            centroidMinimum.fill(std::numeric_limits<double>::max());
            centroidMaximum.fill(std::numeric_limits<double>::lowest());
            for (std::uint32_t i = begin; i < end; ++i) {
                for (int axis = 0; axis < 3; ++axis) {
                    node.minimum[axis] = qMin(node.minimum[axis], rayTriangles[i].minimum[axis]);
                    node.maximum[axis] = qMax(node.maximum[axis], rayTriangles[i].maximum[axis]);
                    centroidMinimum[axis] = qMin(
                        centroidMinimum[axis], rayTriangles[i].centroid[axis]);
                    centroidMaximum[axis] = qMax(
                        centroidMaximum[axis], rayTriangles[i].centroid[axis]);
                }
            }
            const std::int32_t index = static_cast<std::int32_t>(rayNodes.size());
            rayNodes.push_back(node);
            const std::uint32_t count = end - begin;
            if (count <= leafSize) {
                rayNodes[index].firstTriangle = begin;
                rayNodes[index].triangleCount = count;
                return index;
            }
            int splitAxis = 0;
            for (int axis = 1; axis < 3; ++axis) {
                if (centroidMaximum[axis] - centroidMinimum[axis] >
                    centroidMaximum[splitAxis] - centroidMinimum[splitAxis]) splitAxis = axis;
            }
            const std::uint32_t middle = begin + count / 2;
            std::nth_element(rayTriangles.begin() + begin, rayTriangles.begin() + middle,
                             rayTriangles.begin() + end,
                             [splitAxis](const ModelRayTriangleIndexDto& left,
                                         const ModelRayTriangleIndexDto& right) {
                                 return left.centroid[splitAxis] < right.centroid[splitAxis];
                             });
            const std::int32_t left = build(begin, middle);
            if (left < 0) return -1;
            const std::int32_t right = build(middle, end);
            if (right < 0) return -1;
            rayNodes[index].left = left;
            rayNodes[index].right = right;
            return index;
        };
    if (build(0, static_cast<std::uint32_t>(rayTriangles.size())) < 0) {
        rayTriangles.clear();
        rayNodes.clear();
        rayIndexLeafSize = 0;
        return false;
    }
    return true;
}

bool ModelMeshDto::rayHitIndexed(const std::array<double, 3>& origin,
                                 const std::array<double, 3>& direction,
                                 ModelRayHitDto* hit,
                                 ModelRayQueryStatisticsDto* statistics) const
{
    if (hit) *hit = {};
    if (statistics) *statistics = {};
    if (!hit || rayNodes.empty() || rayTriangles.empty()) return false;
    const double directionLengthSquared = direction[0] * direction[0] +
        direction[1] * direction[1] + direction[2] * direction[2];
    if (!std::isfinite(directionLengthSquared) || directionLengthSquared < 1e-18) return false;
    const auto intersectsBounds = [&](const ModelRayNodeIndexDto& node, double maximumDistance) {
        double minimumDistance = 0.0;
        double limit = maximumDistance;
        for (int axis = 0; axis < 3; ++axis) {
            if (qAbs(direction[axis]) < 1e-15) {
                if (origin[axis] < node.minimum[axis] || origin[axis] > node.maximum[axis])
                    return false;
                continue;
            }
            double nearDistance = (node.minimum[axis] - origin[axis]) / direction[axis];
            double farDistance = (node.maximum[axis] - origin[axis]) / direction[axis];
            if (nearDistance > farDistance) std::swap(nearDistance, farDistance);
            minimumDistance = qMax(minimumDistance, nearDistance);
            limit = qMin(limit, farDistance);
            if (minimumDistance > limit) return false;
        }
        return limit >= 0.0;
    };
    const auto worldVertex = [this](std::uint32_t nodeIndex, std::uint32_t globalIndex,
                                    std::array<double, 3>* value) {
        if (!value || globalIndex >= vertices.size()) return false;
        const MeshVertexDto& vertex = vertices[globalIndex];
        *value = {vertex.x, vertex.y, vertex.z};
        if (nodeIndex >= nodes.size()) return true;
        const auto& m = nodes[nodeIndex].worldTransform;
        *value = {
            m[0] * vertex.x + m[4] * vertex.y + m[8] * vertex.z + m[12],
            m[1] * vertex.x + m[5] * vertex.y + m[9] * vertex.z + m[13],
            m[2] * vertex.x + m[6] * vertex.y + m[10] * vertex.z + m[14]};
        return true;
    };
    const auto subtract = [](const std::array<double, 3>& a,
                             const std::array<double, 3>& b) {
        return std::array<double, 3> {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    const auto cross = [](const std::array<double, 3>& a,
                          const std::array<double, 3>& b) {
        return std::array<double, 3> {
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
    };
    const auto dot = [](const std::array<double, 3>& a,
                        const std::array<double, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    bool found = false;
    double nearest = std::numeric_limits<double>::max();
    std::vector<std::int32_t> stack {0};
    while (!stack.empty()) {
        const std::int32_t nodeIndex = stack.back();
        stack.pop_back();
        if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= rayNodes.size()) continue;
        const ModelRayNodeIndexDto& node = rayNodes[nodeIndex];
        if (statistics) ++statistics->boundsTested;
        if (!intersectsBounds(node, nearest)) continue;
        if (node.triangleCount == 0) {
            if (node.right >= 0) stack.push_back(node.right);
            if (node.left >= 0) stack.push_back(node.left);
            continue;
        }
        for (std::uint32_t offset = 0; offset < node.triangleCount; ++offset) {
            const std::uint32_t indexedTriangle = node.firstTriangle + offset;
            if (indexedTriangle >= rayTriangles.size()) continue;
            if (statistics) ++statistics->trianglesTested;
            const ModelRayTriangleIndexDto& reference = rayTriangles[indexedTriangle];
            if (reference.primitiveIndex >= primitives.size()) continue;
            const ModelPrimitiveDto& primitive = primitives[reference.primitiveIndex];
            const std::uint32_t indexOffset = primitive.firstIndex + reference.triangleIndex * 3;
            if (indexOffset + 2 >= indices.size()) continue;
            std::array<double, 3> a, b, c;
            if (!worldVertex(reference.nodeIndex, indices[indexOffset], &a) ||
                !worldVertex(reference.nodeIndex, indices[indexOffset + 1], &b) ||
                !worldVertex(reference.nodeIndex, indices[indexOffset + 2], &c)) continue;
            const auto edge1 = subtract(b, a);
            const auto edge2 = subtract(c, a);
            const auto p = cross(direction, edge2);
            const double determinant = dot(edge1, p);
            if (qAbs(determinant) < 1e-12) continue;
            const double inverseDeterminant = 1.0 / determinant;
            const auto fromA = subtract(origin, a);
            const double u = dot(fromA, p) * inverseDeterminant;
            if (u < 0.0 || u > 1.0) continue;
            const auto q = cross(fromA, edge1);
            const double v = dot(direction, q) * inverseDeterminant;
            if (v < 0.0 || u + v > 1.0) continue;
            const double distance = dot(edge2, q) * inverseDeterminant;
            if (distance < 0.0 || distance >= nearest || !std::isfinite(distance)) continue;
            nearest = distance;
            found = true;
            hit->nodeIndex = reference.nodeIndex;
            hit->primitiveIndex = reference.primitiveIndex;
            hit->triangleIndex = reference.triangleIndex;
            hit->distance = distance;
            hit->position = {
                origin[0] + direction[0] * distance,
                origin[1] + direction[1] * distance,
                origin[2] + direction[2] * distance};
            hit->hasFeature = featureHitForTriangle(
                hit->primitiveIndex, hit->triangleIndex, &hit->feature);
        }
    }
    return found;
}

} // namespace View3D

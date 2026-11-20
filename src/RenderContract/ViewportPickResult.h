#pragma once

#include <QString>
#include <QVariantMap>

#include <array>
#include <cstdint>

namespace View3D {

struct ViewportPickResult {
    QString renderKey;
    std::uint32_t nodeIndex { 0 };
    std::uint32_t primitiveIndex { 0 };
    std::uint32_t triangleIndex { 0 };
    std::int32_t featureTableIndex { -1 };
    std::int64_t featureId { -1 };
    QVariantMap featureProperties;
    std::array<double, 3> worldPosition {};
    std::array<std::array<double, 3>, 3> triangleVertices {};
    bool hasTriangleVertices { false };
    bool hasWorldPosition { false };
    bool exactTriangle { false };
    bool hasFeature { false };
};

} // namespace View3D

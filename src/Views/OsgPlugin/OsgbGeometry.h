#pragma once

#include <ModelMesh.h>

#include <QString>

#include <array>
#include <memory>
#include <vector>

namespace View3DOsb {

// PagedLOD 的一个槽位：external 为真时内容是子瓦片文件。
struct TileLodSlot {
    double minimum = 0.0;
    double maximum = 0.0;
    QString childFile;
    bool external = false;
};

// 瓦片文件内的一个 LOD 节点，世界坐标已减去数据集原点。
struct TileLodNode {
    QString sourceFile;
    std::array<double, 3> center {};
    double radius = 0.0;
    // 同一文件内嵌套 PagedLOD 的父节点序号；-1 表示顶层。
    int parentIndex = -1;
    // osg::LOD::RangeMode：成果为真时 range 的口径是"包围球在屏上的像素尺寸"，
    // 假时才是"到眼点球心距离（米）"。两份成果确实同时存在，混用会把下钻方向反掉。
    bool pixelSizeMode = false;
    // 注意不要命名为 slots：Qt 把 slots 定义为宏。
    std::vector<TileLodSlot> rangeSlots;
};

// 诊断计数：发黑、缺贴图这类问题只能靠实际数据分布定位，逐项统计后再判断主因。
struct TileCounts {
    int geometries = 0;
    int primitives = 0;
    int tilesEmpty = 0;
    int vertices = 0;
    int verticesNoNormal = 0;
    int texturesSeen = 0;
    int texturesConverted = 0;
    int texturesRejected = 0;
    // osg::Image::getOrigin() 的分布：用于确认成果写入方是否按 OpenGL 行序存图。
    int texturesOriginTopLeft = 0;
    int texturesOriginBottomLeft = 0;
    int texturedPrimitives = 0;
    int texturedPrimitivesNoUv = 0;
    int blackAlbedoPrimitives = 0;
    // 数据没给 CullFace 时按 OSG 默认双面画；这项统计用来看剔背面会不会把模型剔出洞。
    int doubleSidedPrimitives = 0;
    int alphaPrimitives = 0;
    TileCounts& operator+=(const TileCounts& other)
    {
        geometries += other.geometries;
        primitives += other.primitives;
        tilesEmpty += other.tilesEmpty;
        vertices += other.vertices;
        verticesNoNormal += other.verticesNoNormal;
        texturesSeen += other.texturesSeen;
        texturesConverted += other.texturesConverted;
        texturesRejected += other.texturesRejected;
        texturesOriginTopLeft += other.texturesOriginTopLeft;
        texturesOriginBottomLeft += other.texturesOriginBottomLeft;
        texturedPrimitives += other.texturedPrimitives;
        texturedPrimitivesNoUv += other.texturedPrimitivesNoUv;
        blackAlbedoPrimitives += other.blackAlbedoPrimitives;
        doubleSidedPrimitives += other.doubleSidedPrimitives;
        alphaPrimitives += other.alphaPrimitives;
        return *this;
    }
};

// 单个 .osgb 文件的转换结果：文件内几何（PagedLOD 槽 0 的内容）合成一个可上传网格，
// 子瓦片保持为外部引用，由 LOD 调度按需读取。
struct ConvertedTile {
    std::shared_ptr<View3D::ModelMeshDto> mesh;
    std::vector<TileLodNode> lodNodes;
    std::array<double, 3> boundsMinimum {};
    std::array<double, 3> boundsMaximum {};
    bool hasBounds = false;
    // 文件里存在不属于任何 PagedLOD 的几何：它跟随整块瓦片的可见性。
    bool hasRootGeometry = false;
    // 读取成功但文件内没有几何：视为已加载的空瓦片，不再重试。
    bool empty = false;
    double readMilliseconds = 0.0;
    double convertMilliseconds = 0.0;
    std::size_t cpuBytes = 0;
    TileCounts counts;
    QString error;

    bool valid() const noexcept
    {
        return mesh && !mesh->vertices.empty() && !mesh->primitives.empty();
    }
};

// 读取并转换一个瓦片文件；originOffset 用于把大地坐标重定位到局部原点。
bool convertTileFile(const QString& filePath, const std::array<double, 3>& originOffset,
                     ConvertedTile* result);

} // namespace View3DOsb

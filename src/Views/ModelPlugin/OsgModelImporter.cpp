#include "OsgModelImporter.h"

#include <osg/Array>
#include <osg/CullFace>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Node>
#include <osg/NodeVisitor>
#include <osg/PrimitiveSet>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osgDB/ReadFile>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace View3DModel {
namespace {

constexpr int kMaxTextureSide = 8192;
constexpr unsigned int kMaxTextureUnits = 4;

// OSG 场景图是 Z-up；渲染约定 +Y 上、视线朝 -Z。(X,Y,Z)->(X,Z,-Y) 的行列式为 +1，
// 是纯旋转：三角绕序与叉乘法线都不会被翻成镜像。用户显式声明源是 Y-up 时不换轴。
osg::Vec3d toView(const osg::Vec3d& value)
{
    return osg::Vec3d(value.x(), value.z(), -value.y());
}

osg::Vec3d transformDirection(const osg::Vec3d& value, const osg::Matrixd& matrix)
{
    // 方向向量只乘 3x3 部分（OSG 行主序：结果第 i 分量为 sum v_j*M(j,i)）。
    return osg::Vec3d(value.x() * matrix(0, 0) + value.y() * matrix(1, 0) +
                          value.z() * matrix(2, 0),
                      value.x() * matrix(0, 1) + value.y() * matrix(1, 1) +
                          value.z() * matrix(2, 1),
                      value.x() * matrix(0, 2) + value.y() * matrix(1, 2) +
                          value.z() * matrix(2, 2));
}

struct BindResolver {
    std::size_t size = 0;

    std::size_t index(unsigned int sourceIndex) const noexcept
    {
        if (size <= 1)
            return 0;
        return sourceIndex < size ? sourceIndex : size - 1;
    }
};

BindResolver bindingFor(const osg::Array* array)
{
    BindResolver resolver;
    if (array) {
        resolver.size = array->getNumElements();
        if (array->getBinding() == osg::Array::BIND_OVERALL ||
            array->getBinding() == osg::Array::BIND_PER_PRIMITIVE_SET)
            resolver.size = 1;
    } else {
        resolver.size = 1;
    }
    return resolver;
}

// 沿父链自近及远找第一个表态的 CullFace，等价于 OSG 的属性继承。没挂 CullFace 时
// OSG 正反两面都画，而 Ogre 默认剔背面，不继承就会把开壳模型剔出洞。
// Values 位含义（osg::StateAttribute）：OFF=0x0、ON=0x1、INHERIT=0x8。
bool drawsBothFaces(const osg::NodePath& path)
{
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const osg::StateSet* state = (*it)->getStateSet();
        if (!state)
            continue;
        const osg::StateSet::RefAttributePair* pair =
            state->getAttributePair(osg::StateAttribute::CULLFACE);
        if (!pair)
            continue;
        const osg::StateAttribute::GLModeValue value = pair->second;
        if (value & osg::StateAttribute::INHERIT)
            continue;
        if (value & osg::StateAttribute::ON)
            return false;
        return true;
    }
    return true;
}

// 材质与贴图也按继承链取：模型文件通常把 StateSet 挂在节点或 Geode 上，而不是
// 挂在 Geometry 自己上面。父链显式 OFF 时到此为止，不再向上找。
const osg::Material* findMaterial(const osg::NodePath& path)
{
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const osg::StateSet* state = (*it)->getStateSet();
        if (!state)
            continue;
        const osg::StateSet::RefAttributePair* pair =
            state->getAttributePair(osg::StateAttribute::MATERIAL);
        if (!pair || !pair->first.get())
            continue;
        if (pair->second & osg::StateAttribute::INHERIT)
            continue;
        if (!(pair->second & osg::StateAttribute::ON))
            return nullptr;
        return dynamic_cast<const osg::Material*>(pair->first.get());
    }
    return nullptr;
}

const osg::Texture2D* findTexture(const osg::NodePath& path, unsigned int* unit)
{
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const osg::StateSet* state = (*it)->getStateSet();
        if (!state)
            continue;
        for (unsigned int index = 0; index < kMaxTextureUnits; ++index) {
            const auto* texture = dynamic_cast<const osg::Texture2D*>(
                state->getTextureAttribute(index, osg::StateAttribute::TEXTURE));
            if (texture) {
                if (unit)
                    *unit = index;
                return texture;
            }
        }
    }
    return nullptr;
}

osg::Vec4 colourAt(const osg::Array& array, std::size_t index)
{
    if (const auto* four = dynamic_cast<const osg::Vec4Array*>(&array))
        return (*four)[index];
    if (const auto* three = dynamic_cast<const osg::Vec3Array*>(&array)) {
        const osg::Vec3 value = (*three)[index];
        return osg::Vec4(value.x(), value.y(), value.z(), 1.0f);
    }
    return osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
}

QPointF texcoordAt(const osg::Array* array, std::size_t index)
{
    if (!array)
        return QPointF(0.0, 0.0);
    if (const auto* two = dynamic_cast<const osg::Vec2Array*>(array))
        return QPointF((*two)[index].x(), (*two)[index].y());
    if (const auto* three = dynamic_cast<const osg::Vec3Array*>(array)) {
        const osg::Vec3 value = (*three)[index];
        return QPointF(value.x(), value.y());
    }
    if (const auto* four = dynamic_cast<const osg::Vec4Array*>(array)) {
        const osg::Vec4 value = (*four)[index];
        return QPointF(value.x(), value.y());
    }
    return QPointF(0.0, 0.0);
}

// DTO 与 D3D11 纹理都是第 0 行对应 v=0（视觉顶行）；osgdb 按文件顺序解码存行，
// 因此这里不翻行（与倾斜摄影通路一致，翻行会把面推进图集未使用的黑边）。
bool imageToRgba(const osg::Image& image, std::vector<std::byte>* rgba, int* width, int* height)
{
    const int s = image.s();
    const int t = image.t();
    if (s <= 0 || t <= 0 || s > kMaxTextureSide || t > kMaxTextureSide)
        return false;
    if (image.getDataType() != GL_UNSIGNED_BYTE)
        return false;
    int channels = 0;
    switch (image.getPixelFormat()) {
    case GL_RGB:
        channels = 3;
        break;
    case GL_RGBA:
        channels = 4;
        break;
    default:
        return false;
    }
    const int rowBytes = s * channels;
    if (image.getRowSizeInBytes() < static_cast<unsigned int>(rowBytes))
        return false;
    rgba->resize(static_cast<std::size_t>(s) * static_cast<std::size_t>(t) * 4u);
    for (int y = 0; y < t; ++y) {
        const unsigned char* source = image.data(0, y);
        if (!source)
            return false;
        std::byte* destination = rgba->data() + static_cast<std::size_t>(y) * s * 4u;
        for (int x = 0; x < s; ++x) {
            const unsigned char* pixel = source + static_cast<std::size_t>(x) * channels;
            destination[static_cast<std::size_t>(x) * 4u + 0u] = std::byte(pixel[0]);
            destination[static_cast<std::size_t>(x) * 4u + 1u] = std::byte(pixel[1]);
            destination[static_cast<std::size_t>(x) * 4u + 2u] = std::byte(pixel[2]);
            destination[static_cast<std::size_t>(x) * 4u + 3u] =
                channels > 3 ? std::byte(pixel[3]) : std::byte(255);
        }
    }
    *width = s;
    *height = t;
    return true;
}

// osgdb 对贴图常只登记文件名（内联图未解码，或本机没有对应的 osgdb_ 图片插件），
// 这时按模型目录与选项里的搜索路径用 QImage 兜底读一次。
bool loadImageFromFile(const QString& fileName, const QString& modelDirectory,
                       const QStringList& searchPaths, std::vector<std::byte>* rgba, int* width,
                       int* height)
{
    if (fileName.isEmpty())
        return false;
    QStringList candidates;
    candidates << QDir(modelDirectory).filePath(fileName);
    for (const QString& search : searchPaths) {
        candidates << QDir(search).filePath(fileName)
                   << QDir(search).filePath(QFileInfo(fileName).fileName());
    }
    for (const QString& candidate : std::as_const(candidates)) {
        if (!QFileInfo::exists(candidate))
            continue;
        QImage image;
        if (!image.load(candidate))
            continue;
        const QImage converted = image.convertToFormat(QImage::Format_RGBA8888);
        if (converted.isNull() || converted.width() > kMaxTextureSide ||
            converted.height() > kMaxTextureSide)
            continue;
        *width = converted.width();
        *height = converted.height();
        rgba->resize(static_cast<std::size_t>(converted.sizeInBytes()));
        std::memcpy(rgba->data(), converted.constBits(), rgba->size());
        return true;
    }
    return false;
}

void appendTriangles(osg::PrimitiveSet& primitiveSet, std::vector<std::uint32_t>* out)
{
    const unsigned int count = primitiveSet.getNumIndices();
    if (count == 0)
        return;
    // 3.6 的 PrimitiveSet 统一提供 index()：DrawArrays 返回自身序号，
    // DrawElements* 返回元素表里的索引。
    std::vector<unsigned int> indices;
    indices.reserve(count);
    for (unsigned int position = 0; position < count; ++position)
        indices.push_back(primitiveSet.index(position));

    auto triangle = [&](unsigned int a, unsigned int b, unsigned int c) {
        out->push_back(a);
        out->push_back(b);
        out->push_back(c);
    };
    switch (primitiveSet.getMode()) {
    case GL_TRIANGLES:
        for (unsigned int i = 0; i + 2 < indices.size(); i += 3)
            triangle(indices[i], indices[i + 1u], indices[i + 2u]);
        break;
    case GL_TRIANGLE_STRIP:
        for (unsigned int i = 2; i < indices.size(); ++i) {
            if (i % 2u == 0u)
                triangle(indices[i - 2u], indices[i - 1u], indices[i]);
            else
                triangle(indices[i - 1u], indices[i - 2u], indices[i]);
        }
        break;
    case GL_TRIANGLE_FAN:
        for (unsigned int i = 2; i < indices.size(); ++i)
            triangle(indices[0], indices[i - 1u], indices[i]);
        break;
    case GL_QUADS:
        for (unsigned int i = 0; i + 3 < indices.size(); i += 4) {
            triangle(indices[i], indices[i + 1u], indices[i + 2u]);
            triangle(indices[i], indices[i + 2u], indices[i + 3u]);
        }
        break;
    case GL_QUAD_STRIP:
        for (unsigned int i = 0; i + 3 < indices.size(); i += 2) {
            triangle(indices[i], indices[i + 1u], indices[i + 2u]);
            triangle(indices[i + 1u], indices[i + 3u], indices[i + 2u]);
        }
        break;
    case GL_POLYGON:
        // 扇形拆分对凹多边形会鼓包；数据里的多边形基本都是凸面。
        for (unsigned int i = 2; i < indices.size(); ++i)
            triangle(indices[0], indices[i - 1u], indices[i]);
        break;
    default:
        break;
    }
}

struct Counts {
    int primitiveSets = 0;
    int primitiveIndices = 0;
    int geometries = 0;
    int primitives = 0;
    int vertices = 0;
    int geometriesNoNormal = 0;
    int texturesSeen = 0;
    int texturesConverted = 0;
    int texturesRejected = 0;
    int texturesFallbackLoaded = 0;
    int texturesMissing = 0;
    int doubleSidedPrimitives = 0;
    int coloredGeometries = 0;
    int skippedLinePrimitives = 0;
    int pointPrimitives = 0;
    int decimatedPointSets = 0;
};

// 整份文件的网格构建器。顶点烘到世界坐标（缩放 → 减原点 → 换轴），节点变换保持
// 单位阵，与倾斜摄影通路一致：这样射线索引与包围盒直接按顶点坐标成立。
class ModelBuilder {
  public:
    ModelBuilder(const ModelImportOptions& options, const QString& modelDirectory)
        : m_options(options), m_modelDirectory(modelDirectory)
    {
        m_mesh = std::make_shared<ModelMeshDto>();
    }

    ModelMeshDto* mesh() noexcept
    {
        return m_mesh.get();
    }
    std::shared_ptr<ModelMeshDto> sharedMesh() const
    {
        return m_mesh;
    }
    const Counts& counts() const
    {
        return m_counts;
    }
    bool overBudget() const
    {
        return m_mesh->vertices.size() > static_cast<std::size_t>(m_options.vertexBudget);
    }
    bool mergeNodes() const
    {
        return m_options.nodeMode == ModelNodeMode::Merge;
    }

    // 一个 Geode 开一个节点：顶点已烘世界坐标，节点只承载命名与选择粒度。
    void beginNode(const QString& name, int sourceNodeIndex)
    {
        flushNode();
        m_pendingName = name;
        m_pendingSource = sourceNodeIndex;
        m_pendingFirst = static_cast<std::uint32_t>(m_mesh->primitives.size());
    }

    void finish()
    {
        flushNode();
        if (!m_mesh->primitives.empty())
            m_mesh->rebuildRayIndex(m_options.rayIndexLeafSize);
    }

    void addGeometry(osg::Geometry& geometry, const osg::Matrixd& world,
                     const osg::NodePath& path)
    {
        const osg::Vec3Array* positions =
            dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
        if (!positions || positions->empty())
            return;
        ++m_counts.geometries;

        const bool swapAxis = m_options.sourceUpAxis != ModelSourceUpAxis::YUp;
        const double scale = m_options.unitScale;
        const osg::Vec3d origin(m_options.localOrigin[0], m_options.localOrigin[1],
                                m_options.localOrigin[2]);
        const auto place = [&](const osg::Vec3d& local) {
            // OSG 行主序：p_world = p_local * M。
            const osg::Vec3d placed = local * world * scale;
            const osg::Vec3d centred = placed - origin;
            return swapAxis ? toView(centred) : centred;
        };

        std::vector<std::uint32_t> triangles;
        std::vector<std::uint32_t> points;
        for (unsigned int index = 0; index < geometry.getNumPrimitiveSets(); ++index) {
            osg::PrimitiveSet* primitiveSet = geometry.getPrimitiveSet(index);
            if (!primitiveSet)
                continue;
            ++m_counts.primitiveSets;
            m_counts.primitiveIndices += static_cast<int>(primitiveSet->getNumIndices());
            switch (primitiveSet->getMode()) {
            case GL_POINTS:
                for (unsigned int position = 0; position < primitiveSet->getNumIndices();
                     ++position)
                    points.push_back(primitiveSet->index(position));
                break;
            case GL_LINES:
            case GL_LINE_STRIP:
            case GL_LINE_LOOP:
                // 网格契约的拓扑只有三角形与点：画线要由渲染端生成带宽度的条带，
                // 这里不凭空造几何，跳过的数量记进统计并汇总成一条告警。
                ++m_counts.skippedLinePrimitives;
                break;
            default:
                appendTriangles(*primitiveSet, &triangles);
                break;
            }
        }
        if (triangles.empty() && points.empty())
            return;

        unsigned int textureUnit = 0;
        const osg::Texture2D* texture = findTexture(path, &textureUnit);

        const osg::Array* normalSource = geometry.getNormalArray();
        const osg::Array* colorSource = geometry.getColorArray();
        // 贴图上采样取它自己那一层的 uv：贴图挂在 unit 1 却读 unit 0 会整体错位。
        const int uvUnit =
            texture ? std::min<int>(static_cast<int>(textureUnit),
                                    static_cast<int>(geometry.getNumTexCoordArrays()) - 1)
                    : 0;
        const osg::Array* uvSource =
            uvUnit >= 0 ? geometry.getTexCoordArray(static_cast<unsigned int>(uvUnit)) : nullptr;
        const osg::Array* uv1Source =
            geometry.getNumTexCoordArrays() > 1 ? geometry.getTexCoordArray(1) : nullptr;
        const BindResolver normalBind = bindingFor(normalSource);
        const BindResolver colorBind = bindingFor(colorSource);
        const BindResolver uvBind = bindingFor(uvSource);
        const BindResolver uv1Bind = bindingFor(uv1Source);
        const auto* normals = dynamic_cast<const osg::Vec3Array*>(normalSource);
        if (colorSource)
            ++m_counts.coloredGeometries;

        // 缺法线时按面积加权自算：Ogre 的 PBS 对没有法线的 renderable 直接输出纯白，
        // 整张贴图都会被丢掉，所以这不是可选优化而是显示前提。
        std::vector<osg::Vec3d> generated;
        const bool regenerate = !normals || m_options.normalMode == ModelNormalMode::Recalculate;
        if (regenerate && !triangles.empty()) {
            generated.assign(positions->size(), osg::Vec3d(0.0, 0.0, 0.0));
            for (std::size_t corner = 0; corner + 2 < triangles.size(); corner += 3) {
                const osg::Vec3d& a = (*positions)[triangles[corner]];
                const osg::Vec3d& b = (*positions)[triangles[corner + 1u]];
                const osg::Vec3d& c = (*positions)[triangles[corner + 2u]];
                const osg::Vec3d face = (b - a) ^ (c - a);
                for (std::size_t k = 0; k < 3; ++k)
                    generated[triangles[corner + k]] += face;
            }
        }
        if (!normals)
            ++m_counts.geometriesNoNormal;

        const std::vector<std::uint32_t>* emitIndices = &triangles;
        ModelPrimitiveTopology topology = ModelPrimitiveTopology::Triangles;
        if (triangles.empty()) {
            emitIndices = &points;
            topology = ModelPrimitiveTopology::Points;
            if (m_options.pointBudget > 0 &&
                points.size() > static_cast<std::size_t>(m_options.pointBudget)) {
                points = decimatePoints(*positions, points);
                ++m_counts.decimatedPointSets;
            }
            ++m_counts.pointPrimitives;
        }

        const std::size_t firstVertex = m_mesh->vertices.size();
        const std::size_t firstIndex = m_mesh->indices.size();
        QHash<unsigned int, std::uint32_t> remap;
        remap.reserve(static_cast<int>(emitIndices->size()));
        for (const unsigned int sourceIndex : *emitIndices) {
            if (sourceIndex >= positions->size())
                continue;
            auto found = remap.constFind(sourceIndex);
            std::uint32_t targetIndex = 0;
            if (found != remap.constEnd()) {
                targetIndex = *found;
            } else {
                MeshVertexDto vertex;
                const osg::Vec3d placed = place((*positions)[sourceIndex]);
                vertex.x = placed.x();
                vertex.y = placed.y();
                vertex.z = placed.z();
                {
                    osg::Vec3d source(0.0, 0.0, 1.0);
                    if (normals && m_options.normalMode != ModelNormalMode::Recalculate)
                        source = (*normals)[normalBind.index(sourceIndex)];
                    else if (!generated.empty())
                        source = generated[sourceIndex];
                    if (m_options.normalMode == ModelNormalMode::Flip)
                        source = -source;
                    osg::Vec3d rotated = transformDirection(source, world);
                    if (swapAxis)
                        rotated = toView(rotated);
                    if (rotated.length2() > 1e-12)
                        rotated.normalize();
                    else
                        rotated = osg::Vec3d(0.0, 1.0, 0.0);
                    vertex.hasNormal = true;
                    vertex.nx = rotated.x();
                    vertex.ny = rotated.y();
                    vertex.nz = rotated.z();
                }
                if (uvSource) {
                    const QPointF value = texcoordAt(uvSource, uvBind.index(sourceIndex));
                    vertex.hasTexCoord = true;
                    vertex.u = value.x();
                    vertex.v = value.y();
                }
                if (uv1Source) {
                    const QPointF value = texcoordAt(uv1Source, uv1Bind.index(sourceIndex));
                    vertex.hasTexCoord1 = true;
                    vertex.u1 = value.x();
                    vertex.v1 = value.y();
                }
                if (colorSource) {
                    const osg::Vec4 colour = colourAt(*colorSource, colorBind.index(sourceIndex));
                    vertex.hasColor = true;
                    vertex.colorR = static_cast<std::uint8_t>(
                        std::clamp(colour.r() * 255.0, 0.0, 255.0));
                    vertex.colorG = static_cast<std::uint8_t>(
                        std::clamp(colour.g() * 255.0, 0.0, 255.0));
                    vertex.colorB = static_cast<std::uint8_t>(
                        std::clamp(colour.b() * 255.0, 0.0, 255.0));
                    vertex.colorA = static_cast<std::uint8_t>(
                        std::clamp(colour.a() * 255.0, 0.0, 255.0));
                }
                m_mesh->vertices.push_back(vertex);
                targetIndex = static_cast<std::uint32_t>(m_mesh->vertices.size() - 1u);
                remap.insert(sourceIndex, targetIndex);
            }
            m_mesh->indices.push_back(targetIndex);
        }
        if (m_mesh->vertices.size() == firstVertex)
            return;

        ModelPrimitiveDto primitive;
        primitive.firstVertex = static_cast<std::uint32_t>(firstVertex);
        primitive.vertexCount = static_cast<std::uint32_t>(m_mesh->vertices.size() - firstVertex);
        primitive.firstIndex = static_cast<std::uint32_t>(firstIndex);
        primitive.indexCount = static_cast<std::uint32_t>(m_mesh->indices.size() - firstIndex);
        primitive.topology = topology;
        primitive.metallicFactor = 0.0;
        primitive.roughnessFactor = 0.9;
        primitive.doubleSided = topology == ModelPrimitiveTopology::Points ||
                                drawsBothFaces(path);
        if (primitive.doubleSided)
            ++m_counts.doubleSidedPrimitives;

        const osg::Material* material = findMaterial(path);
        if (material) {
            const osg::Vec4 diffuse = material->getDiffuse(osg::Material::FRONT_AND_BACK);
            primitive.baseColorR = diffuse.x();
            primitive.baseColorG = diffuse.y();
            primitive.baseColorB = diffuse.z();
            primitive.baseColorA = diffuse.w();
            const osg::Vec4 emissive = material->getEmission(osg::Material::FRONT_AND_BACK);
            primitive.emissiveR = emissive.x();
            primitive.emissiveG = emissive.y();
            primitive.emissiveB = emissive.z();
            // OSG 数据只有高光指数，按经验压到粗糙度：高光越强表面越光滑，与倾斜
            // 摄影那边固定的 0.9 取同一量级；金属度没有对应项，保持 0。
            const double shininess = material->getShininess(osg::Material::FRONT_AND_BACK);
            primitive.roughnessFactor = std::clamp(1.0 - shininess / 128.0, 0.05, 1.0);
            if (primitive.baseColorA < 0.999)
                primitive.alphaBlend = true;
        }

        primitive.baseColorTextureIndex = acquireTexture(texture);

        m_mesh->primitives.push_back(primitive);
        ++m_counts.primitives;
        m_counts.vertices += static_cast<int>(m_mesh->vertices.size() - firstVertex);
    }

  private:
    void flushNode()
    {
        const std::uint32_t last = static_cast<std::uint32_t>(m_mesh->primitives.size());
        if (last <= m_pendingFirst) {
            m_pendingName.clear();
            return;
        }
        ModelNodeDto node;
        node.name = m_pendingName.isEmpty()
            ? QStringLiteral("node%1").arg(m_mesh->nodes.size() + 1)
            : m_pendingName;
        node.sourceNodeIndex = m_pendingSource;
        node.parentIndex = -1;
        node.firstPrimitive = m_pendingFirst;
        node.primitiveCount = last - m_pendingFirst;
        // 顶点已烘世界坐标，节点变换保持契约里的默认单位阵。
        m_mesh->nodes.push_back(node);
        m_mesh->rootNodes.push_back(static_cast<std::uint32_t>(m_mesh->nodes.size() - 1u));
        m_pendingName.clear();
    }

    std::vector<std::uint32_t> decimatePoints(const osg::Vec3Array& positions,
                                              const std::vector<std::uint32_t>& points)
    {
        // 体素抽稀：点顶点是双精度 DTO，千万级点会直接把内存打满。边长取包围盒最长
        // 边除以预算的立方根，保证结果点数不超过预算量级。
        // 这里不用 std::min/std::max：OSG 的头会拉进 windows.h，min/max 宏会吃掉模板名。
        osg::Vec3d minimum = positions[points.front()];
        osg::Vec3d maximum = minimum;
        for (const unsigned int index : points) {
            for (int axis = 0; axis < 3; ++axis) {
                const double value = positions[index][static_cast<unsigned int>(axis)];
                double& low = minimum[static_cast<unsigned int>(axis)];
                double& high = maximum[static_cast<unsigned int>(axis)];
                if (value < low)
                    low = value;
                if (value > high)
                    high = value;
            }
        }
        const osg::Vec3d size = maximum - minimum;
        double longest = size.x();
        if (size.y() > longest)
            longest = size.y();
        if (size.z() > longest)
            longest = size.z();
        const double budgetRoot = std::cbrt(static_cast<double>(m_options.pointBudget));
        const double voxel = longest > 0.0 ? longest / (budgetRoot > 1.0 ? budgetRoot : 1.0) : 1.0;
        std::unordered_set<std::uint64_t> occupied;
        occupied.reserve(points.size());
        std::vector<std::uint32_t> kept;
        kept.reserve(points.size());
        for (const unsigned int index : points) {
            const osg::Vec3d& point = positions[index];
            std::uint64_t hash = 1469598103934665603ull;
            for (int axis = 0; axis < 3; ++axis) {
                const std::int64_t key =
                    static_cast<std::int64_t>(std::floor(point[static_cast<unsigned int>(axis)] /
                                                          voxel));
                const std::uint64_t bits = static_cast<std::uint64_t>(key);
                for (int byte = 0; byte < 8; ++byte) {
                    hash ^= (bits >> (byte * 8)) & 0xffull;
                    hash *= 1099511628211ull;
                }
            }
            if (!occupied.insert(hash).second)
                continue;
            kept.push_back(index);
        }
        return kept;
    }

    int acquireTexture(const osg::Texture2D* texture)
    {
        if (!texture)
            return -1;
        ++m_counts.texturesSeen;
        osg::Image* image = const_cast<osg::Image*>(texture->getImage());
        if (image) {
            const auto found = m_textures.constFind(image);
            if (found != m_textures.constEnd())
                return *found;
            ModelTextureDto dto;
            std::vector<std::byte> rgba;
            int width = 0;
            int height = 0;
            if (imageToRgba(*image, &rgba, &width, &height)) {
                ++m_counts.texturesConverted;
                dto.width = width;
                dto.height = height;
                dto.rgba8 = std::move(rgba);
                m_mesh->textures.push_back(std::move(dto));
                const int index = static_cast<int>(m_mesh->textures.size() - 1u);
                m_textures.insert(image, index);
                return index;
            }
            ++m_counts.texturesRejected;
        }
        // 内联图缺失或解不出来：按贴图登记的文件名再试一次磁盘。
        if (!image || image->getFileName().empty())
            return -1;
        const QString name = QString::fromUtf8(image->getFileName().c_str());
        ModelTextureDto dto;
        std::vector<std::byte> rgba;
        int width = 0;
        int height = 0;
        if (!loadImageFromFile(name, m_modelDirectory, m_options.textureSearchPaths, &rgba, &width,
                               &height)) {
            ++m_counts.texturesMissing;
            return -1;
        }
        dto.width = width;
        dto.height = height;
        dto.rgba8 = std::move(rgba);
        m_mesh->textures.push_back(std::move(dto));
        ++m_counts.texturesFallbackLoaded;
        return static_cast<int>(m_mesh->textures.size() - 1u);
    }

    ModelImportOptions m_options;
    QString m_modelDirectory;
    std::shared_ptr<ModelMeshDto> m_mesh;
    QHash<const osg::Image*, int> m_textures;
    Counts m_counts;
    QString m_pendingName;
    int m_pendingSource = -1;
    std::uint32_t m_pendingFirst = 0;
};

// 深度优先遍历：世界矩阵 = 路径上各节点矩阵连乘（OSG 行主序：
// v_world = v_local * M_parent * M_node）；ABSOLUTE_RF 的变换给的是绝对坐标，
// 从这里重新开始累积。
class GeometryCollector final : public osg::NodeVisitor {
  public:
    GeometryCollector(ModelBuilder* builder)
        : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN), m_builder(builder)
    {
    }

    void apply(osg::Geode& geode) override
    {
        osg::Matrixd world = osg::Matrixd::identity();
        for (osg::Node* node : getNodePath()) {
            if (isAbsolute(node))
                world = matrixOf(node);
            else
                world = world * matrixOf(node);
        }

        if (!m_builder->mergeNodes()) {
            ++m_geodes;
            m_builder->beginNode(geodeName(geode), m_geodes);
        }
        for (unsigned int index = 0; index < geode.getNumDrawables(); ++index) {
            auto* geometry = dynamic_cast<osg::Geometry*>(geode.getDrawable(index));
            if (geometry)
                m_builder->addGeometry(*geometry, world, getNodePath());
        }
    }

    int geodes() const
    {
        return m_geodes;
    }

  private:
    QString geodeName(osg::Geode& geode)
    {
        QString name = QString::fromUtf8(geode.getName().c_str());
        if (name.isEmpty())
            name = QStringLiteral("geode%1").arg(m_geodes);
        return name;
    }

    static osg::Matrixd matrixOf(osg::Node* node)
    {
        // osg::Node 本身不带矩阵；只有 Transform 子类有，其余按单位阵处理。
        if (auto* transform = dynamic_cast<osg::MatrixTransform*>(node))
            return transform->getMatrix();
        return osg::Matrixd::identity();
    }

    static bool isAbsolute(osg::Node* node)
    {
        const auto* transform = dynamic_cast<const osg::Transform*>(node);
        return transform && transform->getReferenceFrame() != osg::Transform::RELATIVE_RF;
    }

    ModelBuilder* m_builder = nullptr;
    int m_geodes = 0;
};

} // namespace

QStringList OsgModelImporter::suffixes()
{
    // 只收 assimp 覆盖不到的 OSG 原生格式：obj/stl/ply/3ds/dxf/lwo 由 assimp 负责，
    // 两个读取器同时认领同一扩展名会让结果取决于登记顺序。
    return {QStringLiteral("osg"), QStringLiteral("osgt"), QStringLiteral("osgb"),
            QStringLiteral("osga"), QStringLiteral("osgtgz"), QStringLiteral("ive"),
            QStringLiteral("flt"), QStringLiteral("ac"), QStringLiteral("3dc"),
            QStringLiteral("bsp"), QStringLiteral("md2"), QStringLiteral("mdl"),
            QStringLiteral("p3d")};
}

bool OsgModelImporter::supportsSuffix(const QString& suffix)
{
    return suffixes().contains(suffix.toLower());
}

ModelReadResult OsgModelImporter::importFile(const QString& path,
                                             const ModelImportOptions& options) const
{
    ModelReadResult output;
    const QFileInfo info(path);
    try {
        osg::ref_ptr<osg::Node> root =
            osgDB::readNodeFile(info.absoluteFilePath().toLocal8Bit().constData());
        if (!root) {
            output.error.code = QStringLiteral("OSG_FILE_READ_FAILED");
            output.error.message = QStringLiteral("OpenSceneGraph 无法读取该文件");
            output.error.source = path;
            return output;
        }
        ModelBuilder builder(options, info.absolutePath());
        GeometryCollector collector(&builder);
        root->accept(collector);
        builder.finish();

        if (builder.overBudget()) {
            output.error.code = QStringLiteral("OSG_VERTEX_BUDGET_EXCEEDED");
            output.error.message = QStringLiteral("顶点数 %1 超过上限 %2")
                                       .arg(builder.mesh()->vertices.size())
                                       .arg(options.vertexBudget);
            output.error.source = path;
            return output;
        }
        if (builder.mesh()->vertices.empty() || builder.mesh()->primitives.empty()) {
            // 计数进错误信息：只有"没有几何"这句话时，无法区分"文件本身是空的"
            // 与"遍历/画元解析没接上"（后者是本读取器的缺陷）。
            const Counts& empty = builder.counts();
            output.error.code = QStringLiteral("OSG_NO_GEOMETRY");
            output.error.message =
                QStringLiteral("文件内没有可直接显示的几何：geode=%1 geometry=%2 画元组=%3 "
                               "画元索引=%4 线画元跳过=%5")
                    .arg(collector.geodes())
                    .arg(empty.geometries)
                    .arg(empty.primitiveSets)
                    .arg(empty.primitiveIndices)
                    .arg(empty.skippedLinePrimitives);
            output.error.source = path;
            return output;
        }

        const Counts& counts = builder.counts();
        QVariantMap statistics;
        statistics.insert(QStringLiteral("format"), info.suffix().toLower());
        statistics.insert(QStringLiteral("osgGeodes"), collector.geodes());
        statistics.insert(QStringLiteral("geometries"), counts.geometries);
        statistics.insert(QStringLiteral("primitiveSets"), counts.primitiveSets);
        statistics.insert(QStringLiteral("primitiveIndices"), counts.primitiveIndices);
        statistics.insert(QStringLiteral("vertices"), static_cast<qulonglong>(counts.vertices));
        statistics.insert(QStringLiteral("indices"),
                          static_cast<qulonglong>(builder.mesh()->indices.size()));
        statistics.insert(QStringLiteral("primitives"),
                          static_cast<qulonglong>(counts.primitives));
        statistics.insert(QStringLiteral("geometriesNoNormal"), counts.geometriesNoNormal);
        statistics.insert(QStringLiteral("texturesSeen"), counts.texturesSeen);
        statistics.insert(QStringLiteral("texturesConverted"), counts.texturesConverted);
        statistics.insert(QStringLiteral("texturesRejected"), counts.texturesRejected);
        statistics.insert(QStringLiteral("texturesFallbackLoaded"), counts.texturesFallbackLoaded);
        statistics.insert(QStringLiteral("texturesMissing"), counts.texturesMissing);
        statistics.insert(QStringLiteral("doubleSidedPrimitives"), counts.doubleSidedPrimitives);
        statistics.insert(QStringLiteral("coloredGeometries"), counts.coloredGeometries);
        statistics.insert(QStringLiteral("skippedLinePrimitives"), counts.skippedLinePrimitives);
        statistics.insert(QStringLiteral("pointPrimitives"), counts.pointPrimitives);
        statistics.insert(QStringLiteral("decimatedPointSets"), counts.decimatedPointSets);
        statistics.insert(QStringLiteral("nodes"),
                          static_cast<qulonglong>(builder.mesh()->nodes.size()));
        statistics.insert(QStringLiteral("unitScale"), options.unitScale);
        statistics.insert(QStringLiteral("sourceUpAxis"),
                          options.sourceUpAxis == ModelSourceUpAxis::YUp ? QStringLiteral("Y")
                                                                         : QStringLiteral("Z"));
        statistics.insert(QStringLiteral("normalMode"),
                          options.normalMode == ModelNormalMode::Recalculate
                              ? QStringLiteral("recalculate")
                              : options.normalMode == ModelNormalMode::Flip
                                    ? QStringLiteral("flip")
                                    : QStringLiteral("read"));
        statistics.insert(QStringLiteral("nodeMode"),
                          options.nodeMode == ModelNodeMode::Merge ? QStringLiteral("merge")
                                                                    : QStringLiteral("preserve"));
        statistics.insert(QStringLiteral("rayIndexLeafSize"),
                          static_cast<qulonglong>(builder.mesh()->rayIndexLeafSize));
        statistics.insert(QStringLiteral("cpuBytes"),
                          static_cast<qulonglong>(modelMeshCpuBytes(*builder.mesh())));

        if (counts.geometriesNoNormal > 0) {
            output.warnings.append(
                QStringLiteral("%1 个几何体没有法线，已按三角形生成顶点法线（否则 PBS 输出纯白）")
                    .arg(counts.geometriesNoNormal));
        }
        if (counts.skippedLinePrimitives > 0) {
            output.warnings.append(QStringLiteral("已跳过 %1 个线画元（网格契约没有线拓扑）")
                                       .arg(counts.skippedLinePrimitives));
        }
        if (counts.texturesMissing > 0) {
            output.warnings.append(QStringLiteral("%1 张贴图在模型目录与搜索路径里都没找到")
                                       .arg(counts.texturesMissing));
        }
        if (counts.decimatedPointSets > 0) {
            output.warnings.append(QStringLiteral("点集超过预算 %1 点，已按体素抽稀 %2 组")
                                       .arg(options.pointBudget)
                                       .arg(counts.decimatedPointSets));
        }

        output.success = true;
        output.mesh = builder.sharedMesh();
        output.readerId = QStringLiteral("reader.osg");
        output.format = info.suffix().toLower();
        output.statistics = statistics;
        return output;
    } catch (...) {
        output.success = false;
        output.error.code = QStringLiteral("OSG_IMPORT_EXCEPTION");
        output.error.message = QStringLiteral("OpenSceneGraph 读取过程中抛出异常");
        output.error.source = path;
        return output;
    }
}

} // namespace View3DModel

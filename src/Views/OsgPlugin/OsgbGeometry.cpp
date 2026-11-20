#include "OsgbGeometry.h"

#include "OsgbRuntime.h"

#include <osg/Array>
#include <osg/CullFace>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/PagedLOD>
#include <osg/PrimitiveSet>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osgDB/ReadFile>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLibrary>
#include <QtGlobal>

#ifdef Q_OS_WIN
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace {

constexpr std::size_t kMaxTileVertices = 4000000u;
constexpr int kMaxTextureSide = 8192;

// OSGB 成果用的是 EPSG 投影坐标：x=东、y=北、z=上；渲染器约定 +Y 为上、视线朝 -Z。
// (E, N, U) -> (E, U, -N) 的行列式为 +1，是纯旋转：三角面绕序和叉乘法线都不会被翻转，
// 少了负号就会变成镜像，模型左右颠倒。
osg::Vec3d toView(const osg::Vec3d& value)
{
    return osg::Vec3d(value.x(), value.z(), -value.y());
}

// 单精度浮点在千米级坐标上仍有亚毫米精度，但大地坐标必须先减掉原点，再做轴向旋转。
osg::Vec3d toLocal(const osg::Vec3d& value, const std::array<double, 3>& origin)
{
    return toView(osg::Vec3d(value.x() - origin[0], value.y() - origin[1], value.z() - origin[2]));
}

// OSG 只有在 StateSet 里挂了会真正表态的 osg::CullFace 时才启用背面剔除，没挂就正反两面都画；
// 而 Ogre 的 HlmsMacroblock 默认是 CULL_CLOCKWISE。倾斜摄影的开壳面（屋顶、地面、边坡）
// 三角绕序不统一，按单面剔就把模型剔成了洞——DasViewer 的双面观感就来自这条默认差。
// 沿父链自近及远找第一个表态的 CullFace，等价于 OSG 的属性继承。
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
            continue;  // 只是挂上但要求继承，继续往上找
        if (value & osg::StateAttribute::ON)
            return false;  // 数据明确要剔背面 → 单面
        return true;  // OFF：显式关掉剔除
    }
    return true;
}

void growBounds(const osg::Vec3d& point, bool* hasBounds, std::array<double, 3>* minimum,
                std::array<double, 3>* maximum)
{
    const double components[3] = {point.x(), point.y(), point.z()};
    if (!*hasBounds) {
        *hasBounds = true;
        for (int axis = 0; axis < 3; ++axis) {
            (*minimum)[static_cast<std::size_t>(axis)] = components[axis];
            (*maximum)[static_cast<std::size_t>(axis)] = components[axis];
        }
        return;
    }
    for (int axis = 0; axis < 3; ++axis) {
        (*minimum)[static_cast<std::size_t>(axis)] =
            std::min((*minimum)[static_cast<std::size_t>(axis)], components[axis]);
        (*maximum)[static_cast<std::size_t>(axis)] =
            std::max((*maximum)[static_cast<std::size_t>(axis)], components[axis]);
    }
}

// DTO 与 D3D11 纹理都是第 0 行对应 v=0（视觉顶行）。
// OSGB 的影像由 osgdb 按文件顺序解码存行，第 0 行就是视觉顶行，UV 也按同一顺序编写，
// 因此这里不能再翻一次行：翻了会把三角面推进图集未使用的黑边。
// 实测（台北101，街景视角）：翻行 340‰ 近黑、平均亮度 70；不翻 5‰、149。
bool imageToRgba(const osg::Image& image, std::vector<std::byte>* rgba, int* width, int* height)
{
    const int s = image.s();
    const int t = image.t();
    if (s <= 0 || t <= 0 || s > kMaxTextureSide || t > kMaxTextureSide)
        return false;
    const unsigned int format = image.getPixelFormat();
    const unsigned int type = image.getDataType();
    if (type != GL_UNSIGNED_BYTE)
        return false;
    int channels = 0;
    switch (format) {
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

bool imageHasTransparency(const osg::Image& image)
{
    if (image.getPixelFormat() != GL_RGBA || image.s() <= 0 || image.t() <= 0)
        return false;
    const int steps = std::max(1, image.t() / 16);
    for (int y = 0; y < image.t(); y += steps) {
        const unsigned char* row = image.data(0, y);
        for (int x = 0; x + 3 < image.s() * 4; x += 4 * std::max(1, image.s() / 16))
            if (row[x + 3] < 250)
                return true;
    }
    return false;
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

class TileBuilder {
  public:
    TileBuilder(std::array<double, 3> origin, View3DOsb::ConvertedTile* result)
        : m_origin(origin), m_result(result)
    {
        m_mesh = std::make_shared<View3D::ModelMeshDto>();
    }

    View3D::ModelMeshDto* mesh() noexcept { return m_mesh.get(); }

    void addGeometry(osg::Geometry& geometry, const osg::Matrixd& matrix, bool doubleSided)
    {
        const osg::Array* vertexArray = geometry.getVertexArray();
        const osg::Vec3Array* positions = dynamic_cast<const osg::Vec3Array*>(vertexArray);
        if (!positions || positions->empty() || m_mesh->vertices.size() >= kMaxTileVertices)
            return;
        // 3.6 的 osg::Geometry 不再有局部矩阵，变换全部来自节点路径。
        const osg::Matrixd full = matrix;
        const osg::Vec3Array* normals =
            dynamic_cast<const osg::Vec3Array*>(geometry.getNormalArray());
        const osg::Vec2Array* texcoords =
            dynamic_cast<const osg::Vec2Array*>(geometry.getTexCoordArray(0));
        const osg::Array* colors = geometry.getColorArray();
        const BindResolver normalBind = bindingFor(normals);
        const BindResolver uvBind = bindingFor(texcoords);
        const BindResolver colorBind = bindingFor(colors);

        std::vector<std::uint32_t> triangles;
        triangles.reserve(positions->size() / 3u * 3u);
        for (unsigned int index = 0; index < geometry.getNumPrimitiveSets(); ++index) {
            osg::PrimitiveSet* primitiveSet = geometry.getPrimitiveSet(index);
            appendTriangles(*primitiveSet, &triangles);
        }
        if (triangles.empty())
            return;
        ++m_result->counts.geometries;

        // 部分 OSGB 数据集（如 6D 含山）的几何体不带法线数组，而 Ogre 的 PBS 像素
        // 着色器对没有法线的 renderable 直接输出纯白（Pbs/Any/Main/
        // 800.PixelShader_piece_ps.any 的 !hlms_normal 分支），整张贴图都会被丢掉。
        // 所以缺法线时按面积加权从三角形自己算一份顶点法线。
        std::vector<osg::Vec3d> generatedNormals;
        if (!normals) {
            generatedNormals.assign(positions->size(), osg::Vec3d(0.0, 0.0, 0.0));
            for (std::size_t corner = 0; corner + 2 < triangles.size(); corner += 3) {
                const osg::Vec3d& a = (*positions)[triangles[corner]];
                const osg::Vec3d& b = (*positions)[triangles[corner + 1]];
                const osg::Vec3d& c = (*positions)[triangles[corner + 2]];
                const osg::Vec3d face = (b - a) ^ (c - a);
                for (std::size_t k = 0; k < 3; ++k)
                    generatedNormals[triangles[corner + k]] += face;
            }
        }

        const osg::Vec4 diffuse = diffuseColour(geometry);
        const osg::Texture2D* texture = findTexture(geometry);
        int textureIndex = -1;
        bool textureAlpha = false;
        if (texture) {
            textureIndex = acquireTexture(*texture);
            textureAlpha = texture->getImage() && imageHasTransparency(*texture->getImage());
        }

        const std::size_t firstVertex = m_mesh->vertices.size();
        QHash<unsigned int, std::uint32_t> remap;
        remap.reserve(static_cast<int>(triangles.size()));
        const std::size_t firstIndex = m_mesh->indices.size();
        for (const unsigned int sourceIndex : triangles) {
            auto found = remap.constFind(sourceIndex);
            std::uint32_t targetIndex = 0;
            if (found != remap.constEnd()) {
                targetIndex = *found;
            } else {
                View3D::MeshVertexDto vertex;
                const osg::Vec3d world = (*positions)[sourceIndex] * full;
                const osg::Vec3d local = toLocal(world, m_origin);
                vertex.x = local.x();
                vertex.y = local.y();
                vertex.z = local.z();
                {
                    osg::Vec3d source(0.0, 0.0, 1.0);
                    if (normals)
                        source = (*normals)[normalBind.index(sourceIndex)];
                    else
                        source = generatedNormals[sourceIndex];
                    // 方向向量只乘 3x3 部分（OSG 行主序：结果第 i 分量为 sum v_j*M(j,i)）。
                    const osg::Vec3d rotated(
                        source.x() * full(0, 0) + source.y() * full(1, 0) +
                            source.z() * full(2, 0),
                        source.x() * full(0, 1) + source.y() * full(1, 1) +
                            source.z() * full(2, 1),
                        source.x() * full(0, 2) + source.y() * full(1, 2) +
                            source.z() * full(2, 2));
                    osg::Vec3d transformed = toView(rotated);
                    if (transformed.length2() > 1e-12)
                        transformed.normalize();
                    else
                        transformed = osg::Vec3d(0.0, 0.0, 1.0);
                    vertex.hasNormal = true;
                    vertex.nx = transformed.x();
                    vertex.ny = transformed.y();
                    vertex.nz = transformed.z();
                }
                if (texcoords) {
                    const osg::Vec2 value = (*texcoords)[uvBind.index(sourceIndex)];
                    vertex.hasTexCoord = true;
                    vertex.u = value.x();
                    vertex.v = value.y();
                }
                if (colors) {
                    const osg::Vec4 colour = colourAt(*colors, colorBind.index(sourceIndex));
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
                growBounds(local, &m_hasBounds, &m_boundsMinimum, &m_boundsMaximum);
                m_mesh->vertices.push_back(vertex);
                targetIndex = static_cast<std::uint32_t>(m_mesh->vertices.size() - 1u);
                remap.insert(sourceIndex, targetIndex);
            }
            m_mesh->indices.push_back(targetIndex);
        }

        View3D::ModelPrimitiveDto primitive;
        primitive.firstVertex = static_cast<std::uint32_t>(firstVertex);
        primitive.vertexCount = static_cast<std::uint32_t>(m_mesh->vertices.size() - firstVertex);
        primitive.firstIndex = static_cast<std::uint32_t>(firstIndex);
        primitive.indexCount = static_cast<std::uint32_t>(m_mesh->indices.size() - firstIndex);
        primitive.baseColorTextureIndex = textureIndex;
        primitive.baseColorR = diffuse.x() < 0.0 ? 1.0 : diffuse.x();
        primitive.baseColorG = diffuse.y() < 0.0 ? 1.0 : diffuse.y();
        primitive.baseColorB = diffuse.z() < 0.0 ? 1.0 : diffuse.z();
        primitive.baseColorA = diffuse.w() < 0.0 ? 1.0 : diffuse.w();
        primitive.metallicFactor = 0.0;
        primitive.roughnessFactor = 0.9;
        primitive.doubleSided = doubleSided;
        if (textureAlpha || primitive.baseColorA < 0.999) {
            primitive.alphaMask = textureAlpha;
            primitive.alphaCutoff = 0.5;
            primitive.alphaBlend = primitive.baseColorA < 0.999;
        }
        ++m_result->counts.primitives;
        if (primitive.doubleSided)
            ++m_result->counts.doubleSidedPrimitives;
        m_result->counts.vertices += static_cast<int>(m_mesh->vertices.size() - firstVertex);
        if (!normals)
            m_result->counts.verticesNoNormal +=
                static_cast<int>(m_mesh->vertices.size() - firstVertex);
        if (textureIndex >= 0) {
            ++m_result->counts.texturedPrimitives;
            if (!texcoords)
                ++m_result->counts.texturedPrimitivesNoUv;
            if (std::max(std::max(primitive.baseColorR, primitive.baseColorG),
                         primitive.baseColorB) < 0.25)
                ++m_result->counts.blackAlbedoPrimitives;
        }
        if (primitive.alphaMask || primitive.alphaBlend)
            ++m_result->counts.alphaPrimitives;
        // 倾斜摄影的影像是航飞时就把光照烘焙进去的：再乘一次场景光照，整座城市会被
        // 压成黑色剪影。这里走自发光通道（不受方向光与阴影影响），漫反射因子置黑，
        // 贴图仍绑在 diffuse 上以保留 alpha 遮罩。
        if (textureIndex >= 0) {
            primitive.emissiveTextureIndex = textureIndex;
            primitive.emissiveTexCoord = primitive.baseColorTexCoord;
            primitive.emissiveR = 1.0;
            primitive.emissiveG = 1.0;
            primitive.emissiveB = 1.0;
            primitive.baseColorR = 0.0;
            primitive.baseColorG = 0.0;
            primitive.baseColorB = 0.0;
        }
        m_mesh->primitives.push_back(primitive);
        // 记录这条 primitive 属于哪个 PagedLOD 组：调度器按组下发可见性，
        // 组外的几何（文件自身内容）记 -1，跟随整块瓦片的可见性。
        m_primitiveOwners.push_back(m_lodStack.empty() ? -1 : m_lodStack.back());
    }

    int addLodNode(osg::PagedLOD& lod, const osg::Matrixd& matrix, const QString& sourceFile)
    {
        View3DOsb::TileLodNode node;
        node.sourceFile = sourceFile;
        node.parentIndex = m_lodStack.empty() ? -1 : m_lodStack.back();
        const osg::BoundingSphere sphere = lod.getBound();
        const osg::Vec3d worldCentre = osg::Vec3d(sphere.center()) * matrix;
        const osg::Vec3d localCentre = toLocal(worldCentre, m_origin);
        node.center = {localCentre.x(), localCentre.y(), localCentre.z()};
        // 取 3x3 列向量的最大长度作为尺度估计，避免依赖具体版本的 decompose 重载。
        double columnScale = 0.0;
        for (int column = 0; column < 3; ++column) {
            double sum = 0.0;
            for (int row = 0; row < 3; ++row) {
                const double value = matrix(row, column);
                sum += value * value;
            }
            columnScale = std::max(columnScale, std::sqrt(sum));
        }
        node.radius = sphere.radius() * columnScale;
        // osg::LOD::traverse 的选择量由 rangeMode 决定，两种口径的单位不同：
        //   DISTANCE_FROM_EYE_POINT → 到 LOD 中心的距离（米）
        //   PIXEL_SIZE_ON_SCREEN    → 包围球在屏上的投影像素尺寸
        // 成果里两种都有（台北101 用前者，含山/大雁塔 用后者），按米统一比较会把
        // 后者的下钻方向整个反过来：靠近时命中粗的自身几何，远离时才请求细子瓦片。
        node.pixelSizeMode = lod.getRangeMode() == osg::LOD::PIXEL_SIZE_ON_SCREEN;
        // 子文件路径是 databasePath + fileName（旧版读取器同款拼接）：有些成果把
        // 下一级放在兄弟目录里，只按父文件所在目录解析会找不到。
        const std::string databasePath = lod.getDatabasePath();
        const unsigned int ranges = lod.getNumRanges();
        const unsigned int files = lod.getNumFileNames();
        const unsigned int slotCount = std::max(ranges, files);
        for (unsigned int index = 0; index < slotCount; ++index) {
            View3DOsb::TileLodSlot slot;
            slot.minimum = index < ranges ? lod.getMinRange(index) : 0.0;
            slot.maximum = index < ranges ? lod.getMaxRange(index)
                                          : std::numeric_limits<double>::max();
            if (index < files) {
                const std::string& name = lod.getFileName(index);
                slot.childFile = QString::fromStdString(databasePath + name);
                slot.external = !name.empty();
            }
            node.rangeSlots.push_back(slot);
        }
        m_result->lodNodes.push_back(std::move(node));
        return static_cast<int>(m_result->lodNodes.size() - 1);
    }

    // 同一次遍历里按 PagedLOD 入栈/出栈，几何归属最近一层节点。
    // 位掩码只有 64 位，超出的组并到第 63 位，避免越界丢可见性控制。
    void pushLodNode(int index) { m_lodStack.push_back(std::min(index, 63)); }
    void popLodNode()
    {
        if (!m_lodStack.empty())
            m_lodStack.pop_back();
    }

    void finish()
    {
        m_mesh->nodes.clear();
        m_mesh->rootNodes.clear();
        // 连续的同组 primitive 合成一个 ModelNodeDto：渲染器按节点建场景节点，
        // 这样同一个文件里已被子瓦片接管的组可以单独隐藏，不必整份文件退回粗层。
        std::size_t index = 0;
        while (index < m_mesh->primitives.size()) {
            const int owner = index < m_primitiveOwners.size() ? m_primitiveOwners[index] : -1;
            std::size_t end = index + 1;
            while (end < m_mesh->primitives.size() &&
                   (end < m_primitiveOwners.size() ? m_primitiveOwners[end] : -1) == owner)
                ++end;
            View3D::ModelNodeDto node;
            node.name = owner >= 0 ? QStringLiteral("lod%1").arg(owner)
                                   : QStringLiteral("root");
            node.sourceNodeIndex = owner;
            node.meshIndex = 0;
            node.firstPrimitive = static_cast<std::uint32_t>(index);
            node.primitiveCount = static_cast<std::uint32_t>(end - index);
            m_mesh->nodes.push_back(node);
            m_mesh->rootNodes.push_back(
                static_cast<std::uint32_t>(m_mesh->nodes.size() - 1));
            if (owner < 0)
                m_result->hasRootGeometry = true;
            index = end;
        }
        if (m_mesh->nodes.empty()) {
            View3D::ModelNodeDto node;
            node.name = QStringLiteral("osgb");
            node.meshIndex = 0;
            node.firstPrimitive = 0;
            node.primitiveCount = static_cast<std::uint32_t>(m_mesh->primitives.size());
            m_mesh->nodes.push_back(node);
            m_mesh->rootNodes.push_back(0);
        }
        m_result->mesh = m_mesh;
        m_result->hasBounds = m_hasBounds;
        m_result->boundsMinimum = m_boundsMinimum;
        m_result->boundsMaximum = m_boundsMaximum;
        m_result->cpuBytes = modelMeshCpuBytes(*m_mesh);
    }

  private:
    static osg::Vec4 colourAt(const osg::Array& array, std::size_t index)
    {
        if (const auto* four = dynamic_cast<const osg::Vec4Array*>(&array))
            return (*four)[index];
        if (const auto* three = dynamic_cast<const osg::Vec3Array*>(&array)) {
            const osg::Vec3 value = (*three)[index];
            return osg::Vec4(value.x(), value.y(), value.z(), 1.0f);
        }
        return osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    static osg::Vec4 diffuseColour(osg::Geometry& geometry)
    {
        osg::StateSet* state = geometry.getOrCreateStateSet();
        osg::Material* material =
            dynamic_cast<osg::Material*>(state->getAttribute(osg::StateAttribute::MATERIAL));
        if (!material)
            return osg::Vec4(-1.0f, -1.0f, -1.0f, -1.0f);
        return material->getDiffuse(osg::Material::FRONT_AND_BACK);
    }

    static osg::Texture2D* findTexture(osg::Geometry& geometry)
    {
        osg::StateSet* state = geometry.getOrCreateStateSet();
        for (unsigned int unit = 0; unit < 4; ++unit) {
            osg::StateAttribute* attribute =
                state->getTextureAttribute(unit, osg::StateAttribute::TEXTURE);
            if (auto* texture = dynamic_cast<osg::Texture2D*>(attribute)) {
                if (texture->getImage())
                    return texture;
            }
        }
        return nullptr;
    }

    int acquireTexture(const osg::Texture2D& texture)
    {
        osg::Image* image = const_cast<osg::Texture2D&>(texture).getImage();
        if (!image)
            return -1;
        const auto found = m_textures.constFind(image);
        if (found != m_textures.constEnd())
            return *found;
        View3D::ModelTextureDto dto;
        std::vector<std::byte> rgba;
        int width = 0;
        int height = 0;
        ++m_result->counts.texturesSeen;
        if (!imageToRgba(*image, &rgba, &width, &height)) {
            ++m_result->counts.texturesRejected;
            static int s_loggedRejections = 0;
            if (s_loggedRejections < 8) {
                ++s_loggedRejections;
                std::cerr << "VIEW3D_OSBG_TEXREJECT s=" << image->s() << " t=" << image->t()
                          << " format=0x" << std::hex << image->getPixelFormat()
                          << " type=0x" << image->getDataType() << std::dec
                          << " rowBytes=" << image->getRowSizeInBytes() << std::endl;
            }
            return -1;
        }
        ++m_result->counts.texturesConverted;
        if (image->getOrigin() == osg::Image::TOP_LEFT)
            ++m_result->counts.texturesOriginTopLeft;
        else
            ++m_result->counts.texturesOriginBottomLeft;
        dto.width = width;
        dto.height = height;
        dto.rgba8 = std::move(rgba);
        m_mesh->textures.push_back(std::move(dto));
        const int index = static_cast<int>(m_mesh->textures.size() - 1u);
        m_textures.insert(image, index);
        return index;
    }

    static void appendTriangles(osg::PrimitiveSet& primitiveSet,
                                std::vector<std::uint32_t>* out)
    {
        const unsigned int count = primitiveSet.getNumIndices();
        if (count == 0)
            return;
        // 3.6 的 PrimitiveSet 统一提供 index()：DrawArrays 返回自身序号，
        // DrawElements* 返回元素表里的索引。
        const auto vertexIndex = [&primitiveSet](unsigned int position) -> unsigned int {
            return primitiveSet.index(position);
        };
        const GLenum mode = primitiveSet.getMode();
        std::vector<unsigned int> indices;
        indices.reserve(count);
        for (unsigned int position = 0; position < count; ++position)
            indices.push_back(vertexIndex(position));

        auto triangle = [&](unsigned int a, unsigned int b, unsigned int c) {
            out->push_back(a);
            out->push_back(b);
            out->push_back(c);
        };
        switch (mode) {
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
            for (unsigned int i = 2; i < indices.size(); ++i)
                triangle(indices[0], indices[i - 1u], indices[i]);
            break;
        default:
            break;
        }
    }

    std::array<double, 3> m_origin;
    View3DOsb::ConvertedTile* m_result;
    std::shared_ptr<View3D::ModelMeshDto> m_mesh;
    QHash<const osg::Image*, int> m_textures;
    std::vector<int> m_lodStack;
    std::vector<int> m_primitiveOwners;
    bool m_hasBounds = false;
    std::array<double, 3> m_boundsMinimum {};
    std::array<double, 3> m_boundsMaximum {};
};

class Traverse : public osg::NodeVisitor {
  public:
    Traverse(TileBuilder* builder, QString sourceFile)
        : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
          m_builder(builder), m_sourceFile(std::move(sourceFile))
    {
    }

    void pushTransform(osg::Transform& transform)
    {
        osg::Matrixd local;
        if (auto* matrixTransform = dynamic_cast<osg::MatrixTransform*>(&transform))
            local = matrixTransform->getMatrix();
        m_matrices.push_back(m_matrices.back() * local);
    }

    void popTransform() { m_matrices.pop_back(); }

    void apply(osg::Transform& transform) override
    {
        pushTransform(transform);
        traverse(transform);
        popTransform();
    }

    void apply(osg::Geometry& geometry) override
    {
        static const bool forceSingleSided =
            qEnvironmentVariableIsSet("VIEW3D_OSBG_SINGLE_SIDED");
        bool doubleSided = drawsBothFaces(getNodePath());
        if (forceSingleSided)
            doubleSided = false;
        m_builder->addGeometry(geometry, m_matrices.back(), doubleSided);
    }

    void apply(osg::PagedLOD& lod) override
    {
        const int nodeIndex = m_builder->addLodNode(lod, m_matrices.back(), m_sourceFile);
        m_builder->pushLodNode(nodeIndex);
        // 槽 0 是本文件内容，其余槽位是外部子瓦片：只遍历第一个子节点。
        if (lod.getNumChildren() > 0 && lod.getChild(0))
            lod.getChild(0)->accept(*this);
        m_builder->popLodNode();
    }

  private:
    TileBuilder* m_builder = nullptr;
    QString m_sourceFile;
    std::vector<osg::Matrixd> m_matrices { osg::Matrixd::identity() };
};

} // namespace

namespace View3DOsb {

namespace {

// 随包的 OSG 是 OSG_USE_UTF8_FILENAME=ON 构建的（osgDB::ifstream 会转 UTF-16 再开），
// 所以 UTF-8 路径本来就是正解。但换台机器后可能加载到另一份没开这个开关的 osgDB.dll
// （PATH 上有别的 OSG、或拷贝不完整），那时窄路径会按系统内码解释，中文目录直接打不开。
// 所以失败后依次回退：系统内码（GBK）→ 8.3 短路径（纯 ASCII，任何 fopen 都认）。
bool hasNonAscii(const QString& text)
{
    for (const QChar character : text) {
        if (character.unicode() > 0x7F)
            return true;
    }
    return false;
}

#ifdef Q_OS_WIN
QString shortPathOf(const QString& path)
{
    wchar_t buffer[32768];
    const DWORD length = ::GetShortPathNameW(reinterpret_cast<LPCWSTR>(path.utf16()), buffer,
                                             DWORD(32768));
    if (length == 0 || length >= 32768)
        return {};
    return QString::fromWCharArray(buffer, int(length));
}
#endif

QString osgbReaderPathOnDisk()
{
    for (const QString& dir : osgPluginDirs()) {
        const QString candidate = QDir(dir).filePath(QStringLiteral("osgdb_osg.dll"));
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return {};
}

struct ReaderState {
    bool ready = false;
    QString error;
};

// .osgb 是 OSG2 的序列化格式：读取器（osgdb_osg）只负责容器，具体类由
// osgdb_serializers_*.dll 注册。插件目录没拷全时读取器能加载、但构造不出对象，
// 报的正是 "file not handled"——看起来像文件坏了。失败后按绝对路径补齐再试。
bool preloadOsgbSerializers()
{
    static const bool loaded = [] {
        int count = 0;
        for (const QString& dir : osgPluginDirs()) {
            const QDir pluginDir(dir);
            const QStringList files = pluginDir.entryList(
                { QStringLiteral("osgdb_serializers_*.dll") }, QDir::Files, QDir::Name);
            for (const QString& file : files) {
#ifdef Q_OS_WIN
                ::SetLastError(0);
                if (::LoadLibraryW(reinterpret_cast<LPCWSTR>(
                        pluginDir.absoluteFilePath(file).utf16())))
                    ++count;
#endif
            }
        }
        std::cerr << "VIEW3D_OSGB_SERIALIZERS loaded=" << count << std::endl;
        return count > 0;
    }();
    return loaded;
}

// osgDB 自己按 "osgPlugins-<版本>/osgdb_osg<后缀>.dll" 找插件：机器上的版本号/调试后缀
// 与 osgDB 期望的不一致（或 PATH 上另有 OSG）时就加载不到。这里改为按绝对路径把同一份
// 读取器 Load 进来——插件 DLL 里的静态注册会落到同一个 osgDB 注册表，之后就能正常读。
const ReaderState& osgbReaderState()
{
    static const ReaderState state = [] {
        ReaderState result;
        auto* registry = osgDB::Registry::instance();
        if (registry->getReaderWriterForExtension("osgb")) {
            result.ready = true;
            std::cerr << "VIEW3D_OSGB_READER ok=1 preload=0" << std::endl;
            return result;
        }
        const QString pluginPath = osgbReaderPathOnDisk();
        if (pluginPath.isEmpty()) {
            result.error = QStringLiteral("没有找到 osgdb_osg.dll：已查找 %1")
                               .arg(osgPluginSearchHint());
            std::cerr << "VIEW3D_OSGB_READER ok=0 preload=1 reason=missing searched="
                      << osgPluginSearchHint().toStdString() << std::endl;
            return result;
        }
#ifdef Q_OS_WIN
        ::SetLastError(0);
        const HMODULE module = ::LoadLibraryW(reinterpret_cast<LPCWSTR>(pluginPath.utf16()));
        const DWORD code = module ? 0 : ::GetLastError();
        if (!module) {
            result.error = QStringLiteral("osgdb_osg.dll 加载失败：%1（Win32 %2）")
                               .arg(pluginPath)
                               .arg(code);
            std::cerr << "VIEW3D_OSGB_READER ok=0 preload=1 path=" << pluginPath.toStdString()
                      << " win32=" << code << std::endl;
            return result;
        }
#else
        QLibrary preload(pluginPath);
        preload.setLoadHints(QLibrary::PreventUnloadHint);
        if (!preload.load()) {
            result.error = QStringLiteral("osgdb_osg.dll 加载失败：%1（%2）")
                               .arg(pluginPath, preload.errorString());
            std::cerr << "VIEW3D_OSGB_READER ok=0 preload=1 path=" << pluginPath.toStdString()
                      << std::endl;
            return result;
        }
#endif
        result.ready = registry->getReaderWriterForExtension("osgb") != nullptr;
        if (!result.ready) {
            result.error = QStringLiteral(
                               "osgdb_osg.dll 已加载，但 osgDB 没有登记 .osgb 读取器（版本不一致：%1）")
                               .arg(pluginPath);
        }
        std::cerr << "VIEW3D_OSGB_READER ok=" << (result.ready ? 1 : 0) << " preload=1 path="
                  << pluginPath.toStdString() << std::endl;
        return result;
    }();
    return state;
}

osg::ref_ptr<osg::Node> readTileNode(const QString& filePath, QString* diagnosis)
{
    // 先确保 .osgb 读取器可用：osgDB 自己找不到时会退回"按绝对路径加载同一份插件"。
    static_cast<void>(osgbReaderState());
    osg::ref_ptr<osg::Node> node = osgDB::readRefNodeFile(filePath.toStdString());
    if (node)
        return node;
    if (hasNonAscii(filePath)) {
        const QByteArray local = filePath.toLocal8Bit();
        if (!local.isEmpty() && QString::fromLocal8Bit(local) == filePath) {
            node = osgDB::readRefNodeFile(local.constData());
            if (node) {
                std::cerr << "VIEW3D_OSBG_PATH_RETRY mode=ansi path=" << filePath.toStdString()
                          << std::endl;
                return node;
            }
        }
#ifdef Q_OS_WIN
        const QString shortPath = shortPathOf(filePath);
        if (!shortPath.isEmpty() && shortPath != filePath) {
            node = osgDB::readRefNodeFile(shortPath.toStdString());
            if (node) {
                std::cerr << "VIEW3D_OSBG_PATH_RETRY mode=short path=" << filePath.toStdString()
                          << " short=" << shortPath.toStdString() << std::endl;
                return node;
            }
        }
#endif
    }
    // 读取器可用但读不出来：补齐序列化器再重试一次。缺 serializers 时读取器正好
    // 报 "file not handled"（能打开容器、构造不出对象），和文件损坏长得一样。
    if (osgDB::Registry::instance()->getReaderWriterForExtension("osgb") &&
        preloadOsgbSerializers()) {
        node = osgDB::readRefNodeFile(filePath.toStdString());
        if (node) {
            std::cerr << "VIEW3D_OSBG_SERIALIZERS_RETRY ok=1 path=" << filePath.toStdString()
                      << std::endl;
            return node;
        }
    }
    // 全部写法都失败：往下细分，弹窗与 View3D.log 给的是「到底是哪一种」，
    // 换机器排查时不用再猜是路径、数据还是插件部署。
    const bool exists = QFileInfo::exists(filePath);
    QFile probe(filePath);
    const bool readable = probe.open(QIODevice::ReadOnly);
    const qint64 bytes = readable ? probe.size() : -1;
    probe.close();

    QString reason;
    if (auto* reader = osgDB::Registry::instance()->getReaderWriterForExtension("osgb")) {
        if (!exists) {
            reason = QStringLiteral("文件不存在（数据没拷全或路径不对）");
        } else if (!readable) {
            reason = QStringLiteral("文件打不开（权限/被占用/网盘断开）");
        } else if (bytes == 0) {
            reason = QStringLiteral("文件是 0 字节（拷贝没完成）");
        } else {
            // 让读取器自己说话：损坏、版本不符、读到一半断掉都会在这里出现。
            const osgDB::ReaderWriter::ReadResult result =
                reader->readNode(filePath.toStdString(),
                                 osgDB::Registry::instance()->getOptions());
            reason = result.success()
                ? QStringLiteral("osgDB 全局解析失败，单独调用读取器却成功（检查 OSG 文件查找路径）")
                : QStringLiteral("读取器读不了（%1）")
                      .arg(QString::fromStdString(result.statusMessage()));
        }
    } else {
        // 没有读取器：原因由 osgbReaderState() 记录下来（找不到文件 / 加载失败 / 版本不一致）。
        const ReaderState& readerState = osgbReaderState();
        reason = readerState.error.isEmpty() ? QStringLiteral("osgDB 没有登记 .osgb 读取器")
                                             : readerState.error;
    }
    std::cerr << "VIEW3D_OSBG_READ_DIAG path=" << filePath.toStdString()
              << " exists=" << (exists ? 1 : 0) << " readable=" << (readable ? 1 : 0)
              << " bytes=" << bytes << " reason=" << reason.toStdString() << std::endl;
    if (diagnosis)
        *diagnosis = reason;
    return node;
}

} // namespace

bool convertTileFile(const QString& filePath, const std::array<double, 3>& originOffset,
                     ConvertedTile* result)
{
    if (!result)
        return false;
    *result = ConvertedTile {};
    const qint64 readStart = QDateTime::currentMSecsSinceEpoch();
    QString diagnosis;
    osg::ref_ptr<osg::Node> node = readTileNode(filePath, &diagnosis);
    result->readMilliseconds = static_cast<double>(QDateTime::currentMSecsSinceEpoch() -
                                                   readStart);
    if (!node) {
        result->error = QStringLiteral("osgDB 无法读取：%1（%2）")
                            .arg(filePath,
                                 diagnosis.isEmpty() ? QStringLiteral("原因未知") : diagnosis);
        return false;
    }
    TileBuilder builder(originOffset, result);
    const qint64 convertStart = QDateTime::currentMSecsSinceEpoch();
    Traverse traverse(&builder, filePath);
    node->accept(traverse);
    builder.finish();
    result->convertMilliseconds = static_cast<double>(QDateTime::currentMSecsSinceEpoch() -
                                                      convertStart);
    if (!result->valid()) {
        // 读到了文件但没有几何：OSGB 金字塔里常见的空叶瓦片，按“已加载、无内容”处理，
        // 避免每 5 秒重试一次。
        result->empty = true;
        return true;
    }
    return true;
}

} // namespace View3DOsb

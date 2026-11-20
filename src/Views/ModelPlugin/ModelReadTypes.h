#pragma once

#include <ModelMesh.h>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace View3D {
class IGeoReference;
}

namespace View3DModel {

// 导入产物的网格类型直接复用渲染契约，读取器不再定义自己的 DTO。
using View3D::MeshVertexDto;
using View3D::ModelFeatureHitDto;
using View3D::ModelFeaturePropertyDto;
using View3D::ModelFeatureTableDto;
using View3D::ModelMeshDto;
using View3D::ModelNodeDto;
using View3D::ModelPrimitiveDto;
using View3D::ModelPrimitiveTopology;
using View3D::ModelRayHitDto;
using View3D::ModelRayNodeIndexDto;
using View3D::ModelRayQueryStatisticsDto;
using View3D::ModelRayTriangleIndexDto;
using View3D::ModelTangentSource;
using View3D::ModelTextureDto;
using View3D::modelMeshCpuBytes;

// Auto 表示「按该格式的原生上轴处理」：glTF/assimp 按 Y-up，OSG 场景图按 Z-up；
// 用户显式选 Y/Z 时覆盖读取器的默认判断。
enum class ModelSourceUpAxis { Auto, YUp, ZUp };
enum class ModelNormalMode { Read, Recalculate, Flip };
enum class ModelNodeMode { Preserve, Merge };

// 一次导入的全部选项。带地理参考系的读取器（GDAL 矢量/栅格）用它做源参照系
// 重投影；为空时读取器保留源坐标按米/度显示，并在 warnings 里说明。
struct ModelImportOptions {
    // 源数据单位到米的比例；自动等同于按米读取（读取器不再猜单位）。
    double unitScale = 1.0;
    ModelSourceUpAxis sourceUpAxis = ModelSourceUpAxis::Auto;
    ModelNormalMode normalMode = ModelNormalMode::Read;
    ModelNodeMode nodeMode = ModelNodeMode::Preserve;
    QStringList textureSearchPaths;
    std::array<double, 3> localOrigin {0.0, 0.0, 0.0};
    std::uint32_t rayIndexLeafSize = 8;
    // 点云顶点是 double 存储，约 128 字节一个点，不设上限时千万级点云会直接把内存
    // 打满。超出预算按体素抽稀；0 表示不限。
    std::uint64_t pointBudget = 2'000'000;
    // 单个文件允许的最大顶点数；超过即判定失败，避免把整个进程拖死。
    std::uint64_t vertexBudget = 40'000'000;
    // 参照系服务（plugin.view3d.crs 发布）：非空时按源参照系重投影到主坐标系，
    // 并由第一个地理数据源钉住世界原点，因此要非 const 指针。
    View3D::IGeoReference* geoReference = nullptr;
};

// 导入失败的原因。三段式（错误码/说明/来源）保留自数据侧实现：无头检查与日志
// 只按错误码判定，中文说明给人看，来源用于定位是哪个文件。
struct ModelReadError {
    QString code;
    QString message;
    QString source;

    bool isEmpty() const
    {
        return code.isEmpty() && message.isEmpty();
    }
    QString text() const
    {
        QString result = message.isEmpty() ? code : QStringLiteral("%1: %2").arg(code, message);
        if (!source.isEmpty())
            result += QStringLiteral("（%1）").arg(source);
        return result;
    }
};

struct ModelReadResult {
    bool success = false;
    ModelReadError error;
    QStringList warnings;
    std::shared_ptr<ModelMeshDto> mesh;
    // 诊断用：实际命中的读取器 id 与格式名。
    QString readerId;
    QString format;
    // 读取器算出的数据侧统计（格式、点数、原点、坐标系标签等），导入会话按行写日志。
    QVariantMap statistics;
    // 数据自带的源参照系标识（EPSG 代码或 WKT 摘要）：图层树/属性面板据此展示，
    // 已重投影的坐标也能回溯到源定义。
    QString sourceCrs;

    bool valid() const noexcept
    {
        return success && mesh && (!mesh->vertices.empty() && !mesh->primitives.empty());
    }
};

enum class ImportFormatKind { Unknown, Model, PointCloud, Raster, Vector, Tileset, OsgbTiles };

// 一种可以直接显示的三维数据格式。读取器先看内容魔数再看扩展名，这样扩展名
// 写错的文件仍能落到正确的读取器上；而 .json 到底是文本 glTF 还是 3D Tiles
// 清单，也只能靠内容判断。
class IModelFormatReader {
  public:
    virtual ~IModelFormatReader() = default;
    virtual QString id() const = 0;
    // 打开器按类别登记文件对话框过滤器，菜单也按类别分行。
    virtual ImportFormatKind kind() const = 0;
    virtual QStringList suffixes() const = 0;
    virtual bool claim(const QString& path, const QByteArray& prefix) const = 0;
    virtual ModelReadResult read(const QString& path, const ModelImportOptions& options) const = 0;
};

struct ImportFormatProbe {
    QString readerId;
    QString format;
    ImportFormatKind kind = ImportFormatKind::Unknown;
    QStringList diagnostics;
};

// 读取文件头部来判断能否直接显示：先看是不是 3D Tiles 清单，其余交给格式读取器
// 注册表。只看扩展名不够可靠，因为 .json 既可能是清单也可能是文本 glTF。
ImportFormatProbe probeImportFormat(const QString& path);

// 探测窗口的基础长度：足够覆盖 LASF、ASTM E57、ply、glTF、3DS 的魔数以及二进制
// STL 的 80 字节头。JSON 类文件会由 readModelFormatProbeWindow 自动放大窗口。
constexpr qint64 modelFormatProbeBytes = 4096;

QByteArray readModelFormatProbeWindow(const QString& path);

// 内置读取器的登记处。文件对话框过滤器、格式探测和导入调度都从这里取答案，
// 新增格式只需要在 buildBuiltinRegistry() 里追加一行。
class ModelFormatReaderRegistry final {
  public:
    void add(std::unique_ptr<IModelFormatReader> reader);
    // 按登记顺序返回所有认领该文件的读取器 id；空表示没有格式能处理它。
    QStringList readerIdsForPath(const QString& path) const;
    const IModelFormatReader* findReader(const QString& path) const;
    const IModelFormatReader* findReaderById(const QString& readerId) const;
    // 按登记顺序遍历读取器：文件对话框的过滤器分组与类别标签都从这里取。
    QList<const IModelFormatReader*> all() const;

  private:
    std::vector<std::unique_ptr<IModelFormatReader>> m_readers;
};

const ModelFormatReaderRegistry& modelFormatReaders();

// 在指定类别里挑一个能认领该文件的读取器 id；没有则返回空串。文件对话框按类别
// 分行，用户在「模型文件」里点了点云文件时由此决定该交给哪个读取器。
QString selectReaderId(const QString& path, ImportFormatKind kind);

} // namespace View3DModel

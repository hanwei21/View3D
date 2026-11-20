#include "ModelReadTypes.h"

#ifdef VIEW3D_MODEL_HAS_CESIUM
#include "GltfModelImporter.h"
#endif
#ifdef VIEW3D_MODEL_HAS_LAS
#include "LasPointCloudImporter.h"
#endif
#ifdef VIEW3D_MODEL_HAS_E57
#include "E57PointCloudImporter.h"
#endif
#ifdef VIEW3D_MODEL_HAS_GDAL
#include "GdalRasterImporter.h"
#include "GdalVectorImporter.h"
#endif
#ifdef VIEW3D_MODEL_HAS_OSG
#include "OsgModelImporter.h"
#endif
#ifdef VIEW3D_MODEL_HAS_ASSIMP
#include "AssimpModelImporter.h"
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>

namespace View3DModel {
namespace {

QString fileSuffix(const QString& path)
{
    return QFileInfo(path).suffix().toLower();
}

bool startsWithBinaryGltfMagic(const QByteArray& prefix)
{
    return prefix.size() >= 4 && prefix.left(4) == QByteArrayLiteral("glTF");
}

// 文本 glTF 和 3D Tiles 清单都以 {"asset" 开头，只能靠有没有 "root" 区分。这里
// 故意只做子串判断：两种文件都可能比探测窗口大，截断的 JSON 是解析不出来的。
bool looksLikeTextGltf(const QByteArray& prefix)
{
    return prefix.contains("\"asset\"") && !prefix.contains("\"root\"");
}

bool isTilesetManifest(const QByteArray& window)
{
    const QJsonDocument json = QJsonDocument::fromJson(window);
    if (!json.isObject())
        return false;
    const QJsonObject root = json.object();
    return !root.value(QStringLiteral("asset")).toObject().isEmpty() &&
           root.value(QStringLiteral("root")).isObject();
}

// 倾斜摄影数据集由倾斜摄影插件负责：metadata.xml（或同名 Data 目录）出现在同层
// 目录时，本插件让路，否则一个 .osgb 瓦片会被当成整块模型加载，看不到 LOD。
bool isObliqueDataset(const QString& path)
{
    const QFileInfo info(path);
    const QString fileName = info.fileName().compare(QStringLiteral("metadata.xml"),
                                                     Qt::CaseInsensitive) == 0
        ? QFileInfo(path).fileName()
        : QString();
    if (!fileName.isEmpty())
        return true;
    const QDir directory = info.isDir() ? QDir(path) : QDir(info.absolutePath());
    return directory.entryList({QStringLiteral("metadata.xml"), QStringLiteral("Metadata.xml")},
                               QDir::Files)
               .size() > 0;
}

#ifdef VIEW3D_MODEL_HAS_CESIUM
class GltfFormatReader final : public IModelFormatReader {
  public:
    QString id() const override { return QStringLiteral("reader.gltf"); }
    ImportFormatKind kind() const override { return ImportFormatKind::Model; }
    QStringList suffixes() const override
    {
        return {QStringLiteral("glb"), QStringLiteral("gltf")};
    }
    bool claim(const QString& path, const QByteArray& prefix) const override
    {
        if (startsWithBinaryGltfMagic(prefix))
            return true;
        const QString suffix = fileSuffix(path);
        if (suffix == QLatin1String("glb") || suffix == QLatin1String("gltf"))
            return true;
        return suffix == QLatin1String("json") && looksLikeTextGltf(prefix);
    }
    ModelReadResult read(const QString& path, const ModelImportOptions& options) const override
    {
        ModelReadResult result = GltfModelImporter().importFile(path, options);
        result.readerId = id();
        return result;
    }
};
#endif

#ifdef VIEW3D_MODEL_HAS_LAS
class LasFormatReader final : public IModelFormatReader {
  public:
    QString id() const override { return QStringLiteral("reader.las"); }
    ImportFormatKind kind() const override { return ImportFormatKind::PointCloud; }
    QStringList suffixes() const override { return LasPointCloudImporter::suffixes(); }
    bool claim(const QString& path, const QByteArray& prefix) const override
    {
        // LAZ 的头同样是 "LASF"，所以魔数一条判断就覆盖两种压缩与否的情况。
        return LasPointCloudImporter::isCloudMagic(prefix) ||
               LasPointCloudImporter::supportsSuffix(fileSuffix(path));
    }
    ModelReadResult read(const QString& path, const ModelImportOptions& options) const override
    {
        ModelReadResult result = LasPointCloudImporter().importFile(path, options);
        result.readerId = id();
        return result;
    }
};
#endif

#ifdef VIEW3D_MODEL_HAS_E57
class E57FormatReader final : public IModelFormatReader {
  public:
    QString id() const override { return QStringLiteral("reader.e57"); }
    ImportFormatKind kind() const override { return ImportFormatKind::PointCloud; }
    QStringList suffixes() const override { return E57PointCloudImporter::suffixes(); }
    bool claim(const QString& path, const QByteArray& prefix) const override
    {
        // E57 的文件头是固定的 "ASTM-E57" 签名，扩展名写错也能落到这个读取器。
        return E57PointCloudImporter::isCloudMagic(prefix) ||
               E57PointCloudImporter::supportsSuffix(fileSuffix(path));
    }
    ModelReadResult read(const QString& path, const ModelImportOptions& options) const override
    {
        ModelReadResult result = E57PointCloudImporter().importFile(path, options);
        result.readerId = id();
        return result;
    }
};
#endif

#ifdef VIEW3D_MODEL_HAS_GDAL
class GdalRasterFormatReader final : public IModelFormatReader {
  public:
    QString id() const override { return QStringLiteral("reader.gdal.raster"); }
    ImportFormatKind kind() const override { return ImportFormatKind::Raster; }
    QStringList suffixes() const override { return GdalRasterImporter::suffixes(); }
    bool claim(const QString& path, const QByteArray& prefix) const override
    {
        return GdalRasterImporter::isRasterMagic(prefix) ||
               GdalRasterImporter::supportsSuffix(fileSuffix(path));
    }
    ModelReadResult read(const QString& path, const ModelImportOptions& options) const override
    {
        ModelReadResult result = GdalRasterImporter().importFile(path, options);
        result.readerId = id();
        return result;
    }
};

class GdalVectorFormatReader final : public IModelFormatReader {
  public:
    QString id() const override { return QStringLiteral("reader.gdal.vector"); }
    ImportFormatKind kind() const override { return ImportFormatKind::Vector; }
    QStringList suffixes() const override { return GdalVectorImporter::suffixes(); }
    bool claim(const QString& path, const QByteArray& prefix) const override
    {
        return GdalVectorImporter::isVectorMagic(prefix) ||
               GdalVectorImporter::supportsSuffix(fileSuffix(path));
    }
    ModelReadResult read(const QString& path, const ModelImportOptions& options) const override
    {
        ModelReadResult result = GdalVectorImporter().importFile(path, options);
        result.readerId = id();
        return result;
    }
};
#endif

#ifdef VIEW3D_MODEL_HAS_ASSIMP
class AssimpFormatReader final : public IModelFormatReader {
  public:
    QString id() const override { return QStringLiteral("reader.assimp"); }
    ImportFormatKind kind() const override { return ImportFormatKind::Model; }
    QStringList suffixes() const override { return AssimpModelImporter::supportedSuffixes(); }
    bool claim(const QString& path, const QByteArray&) const override
    {
        return AssimpModelImporter::supportsExtension(fileSuffix(path));
    }
    ModelReadResult read(const QString& path, const ModelImportOptions& options) const override
    {
        ModelReadResult result = AssimpModelImporter().importFile(path, options);
        result.readerId = id();
        return result;
    }
};
#endif

#ifdef VIEW3D_MODEL_HAS_OSG
// .osgb 只有在它不属于任何倾斜摄影数据集时才由本读取器整块加载。
class OsgFormatReader final : public IModelFormatReader {
  public:
    QString id() const override { return QStringLiteral("reader.osg"); }
    ImportFormatKind kind() const override { return ImportFormatKind::Model; }
    QStringList suffixes() const override { return OsgModelImporter::suffixes(); }
    bool claim(const QString& path, const QByteArray&) const override
    {
        return !isObliqueDataset(path) && OsgModelImporter::supportsSuffix(fileSuffix(path));
    }
    ModelReadResult read(const QString& path, const ModelImportOptions& options) const override
    {
        ModelReadResult result = OsgModelImporter().importFile(path, options);
        result.readerId = id();
        return result;
    }
};
#endif

ModelFormatReaderRegistry buildBuiltinRegistry()
{
    ModelFormatReaderRegistry registry;
#ifdef VIEW3D_MODEL_HAS_CESIUM
    registry.add(std::make_unique<GltfFormatReader>());
#endif
#ifdef VIEW3D_MODEL_HAS_LAS
    registry.add(std::make_unique<LasFormatReader>());
#endif
#ifdef VIEW3D_MODEL_HAS_E57
    registry.add(std::make_unique<E57FormatReader>());
#endif
#ifdef VIEW3D_MODEL_HAS_GDAL
    registry.add(std::make_unique<GdalRasterFormatReader>());
    registry.add(std::make_unique<GdalVectorFormatReader>());
#endif
#ifdef VIEW3D_MODEL_HAS_ASSIMP
    registry.add(std::make_unique<AssimpFormatReader>());
#endif
#ifdef VIEW3D_MODEL_HAS_OSG
    registry.add(std::make_unique<OsgFormatReader>());
#endif
    return registry;
}

} // namespace

QByteArray readModelFormatProbeWindow(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QByteArray window = file.read(modelFormatProbeBytes);
    const QByteArray trimmed = window.trimmed();
    if (trimmed.startsWith('{') || trimmed.startsWith('<')) {
        // JSON/XML 的判据可能在 4 KB 之后（metadata.xml 的 SRS/ModelMetadata），
        // 放大到 1 MB 再看一次。
        file.seek(0);
        window = file.read(1024 * 1024);
    }
    return window;
}

ImportFormatProbe probeImportFormat(const QString& path)
{
    ImportFormatProbe probe;
    probe.format = fileSuffix(path);
    const QByteArray window = readModelFormatProbeWindow(path);

    if (probe.format == QStringLiteral("json") && isTilesetManifest(window)) {
        probe.kind = ImportFormatKind::Tileset;
        probe.readerId = QStringLiteral("tileset");
        return probe;
    }
    if (probe.format == QStringLiteral("osgb") ||
        probe.format == QStringLiteral("xml")) {
        if (isObliqueDataset(path)) {
            probe.kind = ImportFormatKind::OsgbTiles;
            probe.readerId = QStringLiteral("osgb");
            return probe;
        }
    }

    const ModelFormatReaderRegistry& readers = modelFormatReaders();
    const QStringList claimed = readers.readerIdsForPath(path);
    if (claimed.isEmpty())
        return probe;
    const IModelFormatReader* reader = readers.findReaderById(claimed.front());
    if (!reader)
        return probe;
    probe.readerId = reader->id();
    probe.kind = reader->kind();
    if (claimed.size() > 1) {
        probe.diagnostics.append(
            QStringLiteral("%1 个读取器认领该文件，按登记顺序选用 %2：%3")
                .arg(claimed.size())
                .arg(probe.readerId, path));
    }
    if (window.size() >= 4 && window.left(4) == QByteArrayLiteral("glTF") &&
        probe.format != QStringLiteral("glb")) {
        probe.diagnostics.append(
            QStringLiteral("内容为 GLB，但扩展名为 .%1：%2").arg(probe.format, path));
    }
    return probe;
}

void ModelFormatReaderRegistry::add(std::unique_ptr<IModelFormatReader> reader)
{
    if (reader)
        m_readers.push_back(std::move(reader));
}

QStringList ModelFormatReaderRegistry::readerIdsForPath(const QString& path) const
{
    const QByteArray window = readModelFormatProbeWindow(path);
    QStringList ids;
    for (const auto& reader : m_readers) {
        if (reader->claim(path, window))
            ids.push_back(reader->id());
    }
    return ids;
}

const IModelFormatReader* ModelFormatReaderRegistry::findReader(const QString& path) const
{
    const QByteArray window = readModelFormatProbeWindow(path);
    for (const auto& reader : m_readers) {
        if (reader->claim(path, window))
            return reader.get();
    }
    return nullptr;
}

const IModelFormatReader* ModelFormatReaderRegistry::findReaderById(const QString& readerId) const
{
    for (const auto& reader : m_readers) {
        if (reader->id() == readerId)
            return reader.get();
    }
    return nullptr;
}

QList<const IModelFormatReader*> ModelFormatReaderRegistry::all() const
{
    QList<const IModelFormatReader*> readers;
    readers.reserve(static_cast<int>(m_readers.size()));
    for (const auto& reader : m_readers)
        readers.append(reader.get());
    return readers;
}

const ModelFormatReaderRegistry& modelFormatReaders()
{
    static const ModelFormatReaderRegistry registry = buildBuiltinRegistry();
    return registry;
}

QString selectReaderId(const QString& path, ImportFormatKind kind)
{
    const ImportFormatProbe probe = probeImportFormat(path);
    if (probe.kind == kind && !probe.readerId.isEmpty())
        return probe.readerId;
    // 内容判据优先于过滤器：用户在「模型文件」里挑中点云文件时不该强行按模型读，
    // 交给类别判定，让「所有文件」那条路径去试别的打开器。
    if (probe.kind != ImportFormatKind::Unknown && probe.kind != kind)
        return {};
    const QByteArray window = readModelFormatProbeWindow(path);
    for (const IModelFormatReader* reader : modelFormatReaders().all()) {
        if (reader->kind() == kind && reader->claim(path, window))
            return reader->id();
    }
    return {};
}

} // namespace View3DModel

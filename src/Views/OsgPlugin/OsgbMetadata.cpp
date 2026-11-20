#include "OsgbMetadata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

#include <iostream>

namespace {

constexpr int kMaxRootTiles = 256;

// QXmlStreamReader 只认 UTF-8/UTF-16 系列；Qt6 Core 没有 QTextCodec，第三方部署
// 通常也不带 Qt5Compat。OSGB 成果的 metadata.xml 经常声明 encoding="gb2312"
// （大雁塔那份整 831 字节里一个非 ASCII 字节都没有），声明的编码不被支持时整份文档
// 在 XML 声明处就中止，SRS 与 SRSOrigin 一起丢，数据集原点退化成 (0,0,0)，
// 于是整幅成果按绝对大地坐标渲染——单精度顶点在 3.8e6 米上量化到约 0.25 m。
// 这里按声明编码降级：认得的原样交给 Qt；认不得的把声明改写成 UTF-8 后按字节透传。
// 要读的 SRS/SRSOrigin/DataPath 都是 ASCII 字段；只有把中文写进 DataPath 的成果
// 会退化成乱码，而那条路径解析不到时 locateDataDirectory 会退回 Tile_* 目录扫描。
QByteArray normalizeMetadataBytes(const QByteArray& raw, const QString& filePath)
{
    static const QRegularExpression declarationPattern(
        QStringLiteral("<\\?xml[^?]*\\?>"));
    static const QRegularExpression encodingPattern(
        QStringLiteral("encoding\\s*=\\s*[\"']([^\"']+)[\"']"));
    const QRegularExpressionMatch declaration =
        declarationPattern.match(QString::fromUtf8(raw.left(512)));
    if (!declaration.hasMatch())
        return raw; // 没有声明：Qt 会自行按首字节/BOM 判定，别去动它

    const QString header = declaration.captured(0);
    const QRegularExpressionMatch encoding = encodingPattern.match(header);
    if (!encoding.hasMatch())
        return raw;

    QString key = encoding.captured(1).toLower();
    key.remove(QLatin1Char('-')).remove(QLatin1Char('_')).remove(QLatin1Char(' '));
    static const QStringList supported = {
        QStringLiteral("utf8"), QStringLiteral("utf16"), QStringLiteral("utf16le"),
        QStringLiteral("utf16be"), QStringLiteral("usascii"), QStringLiteral("ascii"),
        QStringLiteral("latin1"), QStringLiteral("iso88591")};
    if (supported.contains(key))
        return raw;

    int nonAscii = 0;
    for (const char byte : raw)
        if (static_cast<unsigned char>(byte) >= 0x80)
            ++nonAscii;
    std::cerr << "VIEW3D_OSBG_META_ENCODING file=" << filePath.toStdString() << " declared="
              << encoding.captured(1).toStdString() << " non_ascii_bytes=" << nonAscii
              << " action=parse_as_utf8" << std::endl;
    return raw.mid(0, declaration.capturedStart())
        + "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        + raw.mid(declaration.capturedEnd());
}

QString firstExistingFile(const QStringList& candidates)
{
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return QDir(candidate).absolutePath();
    }
    return {};
}

// 深度受限的 metadata.xml 搜索：本级 -> ./Data -> 再下一层目录。
QString locateMetadataFile(const QString& directory)
{
    const QStringList direct = {QDir(directory).filePath(QStringLiteral("metadata.xml")),
                                QDir(directory).filePath(QStringLiteral("Metadata.xml")),
                                QDir(directory).filePath(QStringLiteral("Data/metadata.xml")),
                                QDir(directory).filePath(QStringLiteral("Data/Metadata.xml"))};
    const QString found = firstExistingFile(direct);
    if (!found.isEmpty())
        return found;

    QDir root(directory);
    const QStringList subDirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                               QDir::Name);
    for (const QString& sub : subDirs) {
        const QString path = root.filePath(sub);
        QDir iterator(path);
        const QStringList hits = iterator.entryList(
            {QStringLiteral("metadata.xml"), QStringLiteral("Metadata.xml")},
            QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        if (!hits.isEmpty())
            return QDir(path).absoluteFilePath(hits.first());
    }
    return {};
}

void parseMetadata(const QByteArray& content, const QString& filePath,
                   View3DOsb::DatasetDescriptor* result)
{
    QXmlStreamReader xml(content);
    QString declaredDataDir;
    QStringList originParts;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token != QXmlStreamReader::StartElement)
            continue;
        const QString name = xml.name().toString();
        if (name == QLatin1String("SRS")) {
            result->srs = xml.readElementText().trimmed();
        } else if (name == QLatin1String("SRSOrigin")) {
            const QString text = xml.readElementText().trimmed();
            originParts = text.split(QRegularExpression(QStringLiteral("[,;\\s]+")),
                                     Qt::SkipEmptyParts);
        } else if (name == QLatin1String("Data") || name == QLatin1String("DataPath")) {
            const QString text = xml.readElementText().trimmed();
            if (!text.isEmpty() && !text.contains(QLatin1Char('<')))
                declaredDataDir = text;
        }
    }
    // 静默解析失败会让原点退化成 0、整幅成果按大地坐标渲染，症状出现在渲染端、
    // 根因却在一份 XML 上——上一轮就是这么藏的，这里必须把它打印出来。
    if (xml.hasError())
        std::cerr << "VIEW3D_OSBG_META_ERROR file=" << filePath.toStdString()
                  << " reason=" << xml.errorString().toStdString() << std::endl;
    else if (result->srs.isEmpty())
        std::cerr << "VIEW3D_OSBG_META_ERROR file=" << filePath.toStdString()
                  << " reason=没有 SRS 元素" << std::endl;
    else if (originParts.size() < 2)
        std::cerr << "VIEW3D_OSBG_META_ERROR file=" << filePath.toStdString()
                  << " reason=SRSOrigin 字段不足 3 项 parts=" << originParts.size()
                  << std::endl;
    for (int axis = 0; axis < 3 && axis < originParts.size(); ++axis)
        result->origin[static_cast<std::size_t>(axis)] =
            originParts.at(axis).toDouble();
    if (!declaredDataDir.isEmpty()) {
        const QString candidate =
            QDir(QFileInfo(filePath).absolutePath()).filePath(declaredDataDir);
        if (QFileInfo(candidate).isDir())
            result->dataDirectory = QDir(candidate).absolutePath();
    }
}

QString rootTileFileFor(const QDir& tileDir, const QString& tileName)
{
    // 根瓦片的文件名有三种常见形态：<目录名>.osgb（ContextCapture 系，台北101/含山/
    // 大雁塔 都是这种）、Model.osgb、Block.osgb（旧版读取器 ci_osgb_reader 的候选表）。
    // 根文件本身不含 "_L"，靠这一点把层级文件排除掉。
    QStringList candidates;
    candidates << tileName + QStringLiteral(".osgb") << QStringLiteral("Model.osgb")
               << QStringLiteral("Block.osgb");
    for (const QString& candidate : candidates) {
        const QString exact = tileDir.filePath(candidate);
        if (QFileInfo::exists(exact))
            return QFileInfo(exact).absoluteFilePath();
    }
    const QStringList files =
        tileDir.entryList({QStringLiteral("*.osgb")}, QDir::Files, QDir::Name);
    for (const QString& file : files) {
        const QString base = QFileInfo(file).completeBaseName();
        if (!base.contains(QLatin1String("_L")) && !base.contains(QLatin1String("_l")))
            return tileDir.absoluteFilePath(file);
    }
    return {};
}

QString locateDataDirectory(const View3DOsb::DatasetDescriptor& descriptor)
{
    if (!descriptor.dataDirectory.isEmpty() && !QDir(descriptor.dataDirectory).entryList(
            {QStringLiteral("*.osgb")}, QDir::Files).isEmpty())
        return descriptor.dataDirectory;

    const auto looksLikeTileRoots = [](const QDir& dir) {
        if (!dir.entryList({QStringLiteral("Tile_*")}, QDir::Dirs | QDir::NoDotAndDotDot,
                           QDir::Name)
                 .isEmpty())
            return true;
        if (!dir.entryList({QStringLiteral("*.osgb")}, QDir::Files).isEmpty())
            return true;
        // 目录不按 Tile_* 命名时（用块号或中文名的成果）上面两条都落空，只能逐个
        // 子目录看有没有根瓦片文件。这里必须和 collectRootTiles 放宽到同一档，
        // 否则数据目录本身就定位不到，后面的根名候选根本走不到。
        const QStringList subDirs =
            dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& sub : subDirs) {
            if (!rootTileFileFor(QDir(dir.filePath(sub)), sub).isEmpty())
                return true;
        }
        return false;
    };

    const QString metadataDir = QFileInfo(descriptor.metadataFile).absolutePath();
    const QStringList candidates = {metadataDir,
                                   QDir(metadataDir).filePath(QStringLiteral("Data")),
                                   QDir(descriptor.directory).filePath(QStringLiteral("Data")),
                                   descriptor.directory};
    for (const QString& candidate : candidates) {
        QDir dir(candidate);
        if (!dir.exists())
            continue;
        if (looksLikeTileRoots(dir))
            return dir.absolutePath();
    }
    return {};
}

QStringList collectRootTiles(const QString& dataDirectory)
{
    QDir dir(dataDirectory);
    QStringList result;
    // 先按约定前缀扫，扫不到再放宽到"Data 下所有子目录"：有些成果的瓦片目录用块号或
    // 中文名，不带 Tile_ 前缀。放宽那一档靠 rootTileFileFor 的存在性判定兜底，
    // 目录里没有根瓦片文件就不会收进来，所以不会影响现有数据集。
    QStringList tileDirs =
        dir.entryList({QStringLiteral("Tile_*")}, QDir::Dirs | QDir::NoDotAndDotDot,
                      QDir::Name);
    if (tileDirs.isEmpty())
        tileDirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& tile : tileDirs) {
        if (result.size() >= kMaxRootTiles)
            break;
        const QDir tileDir(dir.filePath(tile));
        const QString file = rootTileFileFor(tileDir, tile);
        if (!file.isEmpty())
            result.append(file);
    }
    if (result.isEmpty()) {
        const QStringList loose =
            dir.entryList({QStringLiteral("*.osgb")}, QDir::Files, QDir::Name);
        for (const QString& file : loose) {
            if (result.size() >= kMaxRootTiles)
                break;
            const QString base = QFileInfo(file).completeBaseName();
            if (!base.contains(QLatin1String("_L")))
                result.append(dir.absoluteFilePath(file));
        }
    }
    return result;
}

} // namespace

namespace View3DOsb {

DatasetDescriptor discoverDataset(const QString& directory)
{
    DatasetDescriptor result;
    result.directory = QDir(directory).absolutePath();
    if (!QFileInfo(result.directory).isDir()) {
        result.error = QStringLiteral("目录不存在：%1").arg(directory);
        return result;
    }
    result.metadataFile = locateMetadataFile(result.directory);
    if (result.metadataFile.isEmpty()) {
        result.error = QStringLiteral("未找到 metadata.xml（本级与 Data 内都查过）：%1")
                            .arg(result.directory);
        return result;
    }
    QFile metadataFile(result.metadataFile);
    if (!metadataFile.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("metadata.xml 打不开：%1").arg(result.metadataFile);
        return result;
    }
    const QByteArray content =
        normalizeMetadataBytes(metadataFile.readAll(), result.metadataFile);
    metadataFile.close();
    parseMetadata(content, result.metadataFile, &result);
    result.dataDirectory = locateDataDirectory(result);
    if (result.dataDirectory.isEmpty()) {
        result.error = QStringLiteral("metadata.xml 附近没有找到瓦片目录：%1")
                            .arg(result.metadataFile);
        return result;
    }
    result.rootTileFiles = collectRootTiles(result.dataDirectory);
    if (result.rootTileFiles.isEmpty()) {
        result.error = QStringLiteral("瓦片目录内没有根 .osgb 文件：%1")
                            .arg(result.dataDirectory);
    }
    return result;
}

} // namespace View3DOsb

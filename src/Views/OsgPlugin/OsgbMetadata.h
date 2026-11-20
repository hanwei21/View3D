#pragma once

#include <QString>
#include <QStringList>

#include <array>

namespace View3DOsb {

// 倾斜摄影数据集定位结果：metadata.xml 可能与 Data 同级，也可能在 Data 内部。
struct DatasetDescriptor {
    QString directory;
    QString metadataFile;
    QString dataDirectory;
    QString srs;
    std::array<double, 3> origin {};
    QStringList rootTileFiles;
    QString error;

    bool valid() const noexcept
    {
        return !dataDirectory.isEmpty() && !rootTileFiles.isEmpty();
    }
};

// 在给定目录内发现 metadata.xml、数据目录与根瓦片文件；失败时 error 给出原因。
DatasetDescriptor discoverDataset(const QString& directory);

} // namespace View3DOsb

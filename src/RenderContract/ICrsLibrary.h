#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace View3D {

// 一条参照系定义。definition 是交给 PROJ 的原始文本（EPSG 码、proj 字符串或 WKT），
// 其余字段是解析出来的展示与导出信息。
struct CrsLibraryEntry {
    QString id;          // 稳定标识：编辑与删除按它定位，改名不影响
    QString name;        // 用户可读名
    QString definition;  // EPSG:4547 / +proj=... / WKT
    QString code;        // 解析出的权威码，可空
    QString projString;  // 解析出的 proj4 定义，可空
    QString wkt;         // 解析出的 WKT2 定义，可空
    QString note;        // 备注
    QString origin;      // custom（新建）| import（导入）| 空（手录的权威码）
    bool geographic = false;
    bool projected = false;

    // 列表与下拉里的类型短标签：没有 EPSG 码时也要一眼看得出是哪类参照系。
    QString kindLabel() const
    {
        if (projected)
            return QStringLiteral("投影系");
        if (geographic)
            return QStringLiteral("经纬度");
        return QStringLiteral("自定义");
    }

    // 切投影时该把哪段文本交给后端：有权威码用码，否则用用户给的原定义
    // （EPSG 码 / proj 串 / 完整 WKT 都能解析）。wkt 字段不在这儿用：纯 proj 串
    // 导出的 "WKT2" 只是 CONVERSION[…] 片段，拿它当参照系建不出变换。
    QString effectiveDefinition() const
    {
        if (!code.isEmpty())
            return code;
        return definition;
    }
};

// 自定义参照系列表：新建 / 编辑 / 删除 / 导入 / 导出 / 保存到文件。
// 实现在 plugin.view3d.crs；所有写入路径都会先过 PROJ 校验，认不出的定义不入库，
// 避免留下一条「切投影时才失败」的记录。
class ICrsLibrary {
  public:
    virtual ~ICrsLibrary() = default;

    virtual QList<CrsLibraryEntry> entries() const = 0;
    virtual bool containsDefinition(const QString& definition) const = 0;

    // 名称留空时用 PROJ 解析出的官方名称；added 回填完整条目（含生成的 id）。
    virtual bool add(const QString& name, const QString& definition, const QString& note,
                     CrsLibraryEntry* added, QString* error) = 0;
    virtual bool update(const QString& id, const QString& name, const QString& definition,
                        const QString& note, QString* error) = 0;
    virtual bool remove(const QString& id) = 0;

    // 常用收藏：按定义文本引用，既可以是内置权威码（"EPSG:4547"），也可以是库条目
    // 自己的定义。它决定「显示参照系」下拉里除固定项之外多出来哪些，以及面板顶部
    // 的「常用」分组。首次没有库文件时给一套默认常用（中国工程常用带号）。
    virtual QStringList favorites() const = 0;
    virtual bool isFavorite(const QString& definition) const = 0;
    // 收藏也算库的改动：要点「写入库文件」才落盘，和条目一致。
    virtual void setFavorite(const QString& definition, bool favorite) = 0;

    // 库文件路径与脏标记：dirty 只表示「内存里有改动没落盘」。
    virtual QString filePath() const = 0;
    virtual bool save(QString* error) = 0;
    virtual bool dirty() const = 0;

    // 导入 .prj/.wkt（一个文件一条）与 .json（合并、按定义去重）；返回新增条数，
    // 解析失败的文件名进 *failed，不中断整批。
    virtual int importFiles(const QStringList& paths, QStringList* failed, QString* error) = 0;
    // 导出：单个 id 且后缀是 .prj/.wkt 时只写定义文本（别的 GIS 软件直接认），
    // 其余情况写 JSON 片段库。
    virtual bool exportFile(const QString& path, const QStringList& ids, QString* error) = 0;

    virtual QString lastError() const { return {}; }
};

} // namespace View3D

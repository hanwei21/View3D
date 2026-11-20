#pragma once

#include <IGeoReference.h>

#include <QString>
#include <QStringList>

#include <array>
#include <cstddef>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace View3DCrs {

// 参照系后端：任意两个参照系（EPSG 代码、proj 字符串或 OGC WKT）之间的批量变换。
// 有 PROJ 时走 proj_create_crs_to_crs 并按 (源,目标) 缓存管线，没有 PROJ 时只支持
// 经纬度直通与 EPSG:3857 的内置公式，其余一律返回失败。
class CrsBackend final {
  public:
    CrsBackend();
    ~CrsBackend();

    // dataDir 为空则沿用 PROJ 默认查找路径。
    void initialize(const QString& projDataDir);
    bool available() const noexcept { return m_hasProj; }
    QString lastError() const;

    // 服务接口是 const 的，所以这些方法也按 const 暴露，错误文本走 mutable 成员。
    bool transformToMain(const QString& sourceCrs, double* xyz, std::size_t pointCount) const;
    bool transformBetween(const QString& sourceCrs, const QString& targetCrs, double* xyz,
                          std::size_t pointCount) const;
    bool describe(const QString& sourceCrs, View3D::IGeoReference::CrsInfo* info) const;

    // 定义解析结果：参照系管理要把用户给的 EPSG 码 / proj 字符串 / WKT 归一成
    // 「有名字、有多种导出文本」的一条库记录。
    struct CrsDefinition {
        QString name;        // 权威名称（PROJ 数据库或 WKT 里的 NAME）
        QString code;        // 形如 "EPSG:4547"；自定义 proj 串时为空
        QString projString;  // 单行 +proj=... 定义
        QString wkt;         // WKT2（2019）定义
        QString kind;        // 中文类型标签：地理系/投影系/地心系
        bool geographic = false;
        bool projected = false;
    };
    // 解析任意参照系文本并导出它的各种表示；认不出来返回 false。
    bool inspect(const QString& crsText, CrsDefinition* out) const;
    // 详情面板用的完整描述：比 CrsDefinition 多椭球、基准、用途、备注与使用范围。
    struct CrsDetail {
        QString name;
        QString code;              // 权威码，如 "EPSG:4547"；自定义定义为空
        QString kind;              // 中文类型标签
        QString projectionMethod;  // 投影方法名，地理系为空
        QString ellipsoid;
        QString datum;
        QString scope;
        QString remarks;
        QString projString;
        QString wkt;
        QString areaName;
        bool geographic = false;
        bool projected = false;
        bool deprecated = false;
        bool hasArea = false;
        double west = 0.0;
        double south = 0.0;
        double east = 0.0;
        double north = 0.0;
    };
    // 任意参照系文本（权威码 / proj 串 / WKT）的详情；认不出返回 false。
    bool detail(const QString& crsText, CrsDetail* out) const;

    // 参照系标识：优先权威码（EPSG:xxxx），取不到再退化到坐标系名称。
    static QString labelOf(const QString& crsText, const QString& name);

    // 参照系是否有平面轴：地理系与投影系可以当显示参照系，地心直角不行。
    bool isDisplayable(const QString& crs, bool* projected) const;

    // 权威参照系库（PROJ 自带的 proj.db，和 QGIS 读的是同一份数据）里的概要。
    // 只带列表页要显示的字段，完整定义按 authName:code 再 inspect 一次。
    struct CrsSummary {
        QString authName;          // "EPSG"
        QString code;              // "4547"
        QString name;              // 官方名，如 "CGCS2000 / 3-degree Gauss-Kruger CM 114E"
        QString projectionMethod;  // 投影方法名，地理系为空
        QString areaName;          // 使用范围名，可能为空
        bool projected = false;
        bool geographic = false;
        bool geographic3d = false;   // 经纬度+椭球高：E:4979 这类名字里看不出 3D
        bool deprecated = false;
        bool hasArea = false;
        double west = 0.0;
        double south = 0.0;
        double east = 0.0;
        double north = 0.0;

        QString authorityCode() const
        {
            return authName.isEmpty() ? code : authName + QLatin1Char(':') + code;
        }
    };

    // 库里可用的机构名（EPSG / ESRI / …）；PROJ 或 proj.db 缺失时返回空。
    QStringList authorities() const;
    // 枚举某机构的地理系与投影系。条数是几千级，调用方自己决定何时懒加载。
    bool listAuthorityCrs(const QString& authName, bool allowDeprecated,
                          std::vector<CrsSummary>* out) const;

  private:
    // 管线句柄是 PJ*（PROJ 类型在本头文件里保持不透明），缓存键为 (源,目标) 字符串。
    // 端点是裸 proj 字符串时 proj_create_crs_to_crs 不认，只能自己组
    // "+proj=pipeline …"，那种管线的地理端用弧度，所以要记换算方向。
    struct Pipeline {
        void* pj = nullptr;
        bool radiansFromSource = false;
        bool radiansToTarget = false;
    };
    mutable std::mutex m_mutex;
    mutable std::map<std::pair<QString, QString>, Pipeline> m_pipelines;
    mutable QString m_lastError;
    bool m_hasProj = false;
    void* m_context = nullptr;   // PJ_CONTEXT*，避免污染全局 proj_context_default
};

} // namespace View3DCrs

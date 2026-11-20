#pragma once

#include <QList>
#include <QString>

namespace View3D {

// 内置参照系的浏览入口：直接读 PROJ 自带的权威库 proj.db，也就是 QGIS 用的同一份
// 数据（EPSG 机构有六千多条地理系/投影系）。内置一律只读，要改先复制进「自定义」。
// 实现在 plugin.view3d.crs 里，第一次访问 groups() 才真的枚举。
class ICrsBrowser {
  public:
    virtual ~ICrsBrowser() = default;

    struct Item {
        QString key;         // "EPSG:4547"：选中项、收藏、下拉都用它定位
        QString name;        // 官方名，如 "CGCS2000 / 3-degree Gauss-Kruger CM 114E"
        QString authority;   // "EPSG"
        QString code;        // "4547"
        QString method;      // 投影方法的中文标签，地理系为空
        QString areaName;    // 使用范围名，可能为空
        bool projected = false;
        bool geographic = false;
        bool deprecated = false;
        bool hasArea = false;
        double west = 0.0;
        double south = 0.0;
        double east = 0.0;
        double north = 0.0;
    };

    struct Group {
        QString id;     // 稳定标识，面板按它定位分组
        QString label;  // 分组显示名
        QList<Item> items;
    };

    // 第一次调用会枚举权威库（本机实测 6215 条约 80 ms），之后复用同一份结果。
    virtual const QList<Group>& groups() = 0;
    // proj.db 是否可用；不可用时面板要说明原因而不是给出一个空列表。
    virtual bool available() const = 0;
    // 给面板状态行的一句话：条数统计或失败原因。
    virtual QString statusText() const = 0;
    virtual bool item(const QString& key, Item* out) const = 0;

    // 结构化详情：面板排版、新建对话框的实时预览和「复制」按钮都要原始字段，
    // 所以这里给字段而不是一段拼好的文本。definition 可以是 "EPSG:4547"，
    // 也可以是自定义 proj 串 / WKT；解析不出来返回 false，错误在 lastError()。
    struct Info {
        QString name;
        QString code;      // "EPSG:4547"；自定义定义为空
        QString kind;      // 中文类型标签：地理参考系 / 投影参考系 / …
        QString method;    // 投影方法名，地理系为空
        QString datum;
        QString ellipsoid;
        QString areaName;
        QString scope;
        QString remarks;
        QString projString;
        QString wkt;
        bool geographic = false;
        bool projected = false;
        bool deprecated = false;
        bool hasArea = false;
        double west = 0.0;
        double south = 0.0;
        double east = 0.0;
        double north = 0.0;
    };
    virtual bool info(const QString& definition, Info* out) const = 0;
    virtual QString lastError() const = 0;
};

} // namespace View3D

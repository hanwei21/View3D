#pragma once

#include "PEGlobals.h"

#include <QHash>
#include <QList>
#include <QStringList>
#include <QString>
#include <QMap>

namespace View3D {

struct DockLayoutEntry {
    QString name;
    QString text;
    QString pluginId;
    QString factoryId;
    QString area;
    QString icon;
    bool visible = true;
    bool floating = false;
    bool floatable = true;
    int preferredWidth = 0;
    int preferredHeight = 0;
    QMap<QString, QString> attributes;
};

struct ContentsViewLayoutEntry {
    QString name;
    QString text;
    QString pluginId;
    QString factoryId;
    QString defaultHost;
    QString icon;
    QMap<QString, QString> attributes;
};

struct RibbonGroupLayout {
    QString name;
    QString text;
    struct Item {
        struct Option {
            QString value;
            QString caption;
            QString group;
        };
        QString name;
        QString pluginId;
        QString factoryId;
        QString caption;
        QString hint;
        QString type;
        QString icon;
        QString iconSize;
        QString exclusiveGroup;
        QString shortcut;
        bool checked = false;
        bool enabled = true;
        bool beginGroup = false;
        QList<Option> options;
        QMap<QString, QString> attributes;
    };
    QList<Item> items;
};

struct PackageLayout {
    QString id;
    QString path;
    bool required = false;
    bool enabled = true;
    QStringList requiresPlugins;
    QStringList optionalPlugins;
    QStringList provides;
};

struct RibbonPageLayout {
    QString name;
    QString text;
    QString context;
    QList<RibbonGroupLayout> groups;
};

struct PLUGIN_ENGINE_EXPORT LayoutConfig {
    int windowWidth = 1280;
    int windowHeight = 800;
    int windowX = -1;
    int windowY = -1;
    bool maximized = false;
    QString caption = QStringLiteral("View3D 三维视图");
    QString ribbonStyle = QStringLiteral("office");
    QString ribbonTheme = QStringLiteral("light");
    QList<RibbonGroupLayout::Item> ribbonRightButtons;
    QStringList libraryPaths;
    QList<PackageLayout> packages;
    QHash<QString, QString> welcomeScreen;
    QHash<QString, QString> ribbonLabels;
    QList<RibbonPageLayout> ribbonPages;
    QList<DockLayoutEntry> docks;
    QList<ContentsViewLayoutEntry> contentsViews;

    static LayoutConfig load(const QString& filePath, QString* warning = nullptr);
    QString label(const QString& key, const QString& fallback) const;
};

} // namespace View3D

#include "LayoutConfig.h"

#include <QDateTime>
#include <QFile>
#include <QXmlStreamReader>

namespace View3D {
namespace {

bool xmlBool(const QString& value, bool fallback)
{
    if (value.isEmpty()) return fallback;
    return value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
        value == QStringLiteral("1") ||
        value.compare(QStringLiteral("visible"), Qt::CaseInsensitive) == 0;
}

double xmlNumber(const QString& value, double fallback)
{
    bool ok = false;
    const double result = value.toDouble(&ok);
    return ok ? result : fallback;
}

QStringList xmlList(const QString& value)
{
    QStringList result;
    for (const QString& item : value.split(QLatin1Char(','), Qt::SkipEmptyParts))
        result.append(item.trimmed());
    return result;
}

RibbonGroupLayout::Item parseRibbonItem(QXmlStreamReader& xml)
{
    const QXmlStreamAttributes attributes = xml.attributes();
    RibbonGroupLayout::Item item;
    item.name = attributes.value(QStringLiteral("name")).toString();
    item.pluginId = attributes.value(QStringLiteral("pluginId")).toString();
    item.factoryId = attributes.value(QStringLiteral("factoryId")).toString();
    item.caption = attributes.value(QStringLiteral("caption")).toString();
    item.hint = attributes.value(QStringLiteral("hint")).toString();
    item.type = attributes.value(QStringLiteral("type")).toString();
    item.icon = attributes.value(QStringLiteral("icon")).toString();
    item.iconSize = attributes.value(QStringLiteral("iconSize")).toString();
    item.exclusiveGroup = attributes.value(QStringLiteral("exclusiveGroup")).toString();
    item.shortcut = attributes.value(QStringLiteral("shortcut")).toString();
    item.checked = xmlBool(attributes.value(QStringLiteral("checked")).toString(), false);
    item.enabled = xmlBool(attributes.value(QStringLiteral("enabled")).toString(), true);
    item.beginGroup = xmlBool(
        attributes.value(QStringLiteral("begingroup")).toString(), false);
    for (const QXmlStreamAttribute& attribute : attributes)
        item.attributes.insert(attribute.name().toString(), attribute.value().toString());
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == QStringLiteral("option")) {
                RibbonGroupLayout::Item::Option option;
                option.value = xml.attributes().value(QStringLiteral("value")).toString();
                option.caption = xml.attributes().value(QStringLiteral("text")).toString();
                option.group = xml.attributes().value(QStringLiteral("group")).toString();
                if (!option.value.isEmpty())
                    item.options.append(option);
            }
            xml.skipCurrentElement();
        } else if (xml.isEndElement()) {
            break;
        } else if (xml.hasError()) {
            break;
        }
    }
    return item;
}

} // namespace

LayoutConfig LayoutConfig::load(const QString& filePath, QString* warning)
{
    LayoutConfig config;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (warning) *warning = QStringLiteral("无法读取界面配置，使用内置默认值：%1").arg(filePath);
        return config;
    }

    QXmlStreamReader xml(&file);
    bool inRibbonPages = false;
    bool inRightButtons = false;
    int currentPageIndex = -1;
    int currentGroupIndex = -1;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView tag = xml.name();
            const auto attributes = xml.attributes();
            if (tag == QStringLiteral("path")) {
                const QString value = attributes.value(QStringLiteral("value")).toString();
                if (!value.isEmpty()) config.libraryPaths.append(value);
            } else if (tag == QStringLiteral("package")) {
                PackageLayout package;
                package.id = attributes.value(QStringLiteral("id")).toString();
                package.path = attributes.value(QStringLiteral("path")).toString();
                package.required = xmlBool(attributes.value(QStringLiteral("required")).toString(), false);
                package.enabled = xmlBool(attributes.value(QStringLiteral("enable")).toString(), true);
                package.requiresPlugins = xmlList(attributes.value(QStringLiteral("requires")).toString());
                package.optionalPlugins = xmlList(attributes.value(QStringLiteral("optionalRequires")).toString());
                package.provides = xmlList(attributes.value(QStringLiteral("provides")).toString());
                config.packages.append(package);
            } else if (tag == QStringLiteral("mainapp")) {
                const QString caption = attributes.value(QStringLiteral("caption")).toString();
                if (!caption.isEmpty()) config.caption = caption;
                config.windowWidth = qMax(800, static_cast<int>(xmlNumber(
                    attributes.value(QStringLiteral("width")).toString(), config.windowWidth)));
                config.windowHeight = qMax(600, static_cast<int>(xmlNumber(
                    attributes.value(QStringLiteral("height")).toString(), config.windowHeight)));
                config.windowX = static_cast<int>(xmlNumber(
                    attributes.value(QStringLiteral("x")).toString(), -1));
                config.windowY = static_cast<int>(xmlNumber(
                    attributes.value(QStringLiteral("y")).toString(), -1));
                config.maximized = attributes.value(QStringLiteral("windowstate"))
                    .compare(QStringLiteral("maximized"), Qt::CaseInsensitive) == 0;
            } else if (tag == QStringLiteral("welcomescreen")) {
                for (const QXmlStreamAttribute& attribute : attributes)
                    config.welcomeScreen.insert(attribute.name().toString(),
                                                attribute.value().toString());
            } else if (tag == QStringLiteral("ribboncontrol")) {
                const QString style = attributes.value(QStringLiteral("style")).toString().trimmed();
                const QString theme = attributes.value(QStringLiteral("theme")).toString().trimmed();
                if (!style.isEmpty()) config.ribbonStyle = style;
                if (!theme.isEmpty()) config.ribbonTheme = theme;
            } else if (tag == QStringLiteral("ribbonpages")) {
                inRibbonPages = true;
            } else if (tag == QStringLiteral("rightbuttons")) {
                inRightButtons = true;
            } else if (inRightButtons && tag == QStringLiteral("item")) {
                const RibbonGroupLayout::Item item = parseRibbonItem(xml);
                if (!item.name.isEmpty())
                    config.ribbonRightButtons.append(item);
            } else if (inRibbonPages && tag == QStringLiteral("ribbonpage")) {
                const QString currentPage = attributes.value(QStringLiteral("name")).toString();
                const QString text = attributes.value(QStringLiteral("text")).toString();
                if (!currentPage.isEmpty()) {
                    config.ribbonLabels.insert(currentPage, text);
                    config.ribbonPages.append(
                        {currentPage, text,
                         attributes.value(QStringLiteral("context")).toString(), {}});
                    currentPageIndex = config.ribbonPages.size() - 1;
                    currentGroupIndex = -1;
                }
            } else if (inRibbonPages && tag == QStringLiteral("ribbonpagegroup")) {
                const QString name = attributes.value(QStringLiteral("name")).toString();
                const QString text = attributes.value(QStringLiteral("text")).toString();
                if (!name.isEmpty()) {
                    config.ribbonLabels.insert(name, text);
                    if (currentPageIndex >= 0)
                        config.ribbonPages[currentPageIndex].groups.append({name, text, {}});
                    currentGroupIndex = currentPageIndex >= 0
                        ? config.ribbonPages[currentPageIndex].groups.size() - 1 : -1;
                }
            } else if (inRibbonPages && tag == QStringLiteral("item") &&
                       currentPageIndex >= 0 && currentGroupIndex >= 0) {
                const RibbonGroupLayout::Item item = parseRibbonItem(xml);
                if (!item.name.isEmpty())
                    config.ribbonPages[currentPageIndex].groups[currentGroupIndex].items.append(item);
            } else if (tag == QStringLiteral("dockwindow")) {
                const QString name = attributes.value(QStringLiteral("name")).toString();
                if (name.isEmpty()) continue;
                DockLayoutEntry entry;
                entry.name = name;
                entry.text = attributes.value(QStringLiteral("text")).toString();
                entry.pluginId = attributes.value(QStringLiteral("pluginId")).toString();
                entry.factoryId = attributes.value(QStringLiteral("factoryId")).toString();
                entry.area = attributes.value(QStringLiteral("dockpos")).toString();
                entry.icon = attributes.value(QStringLiteral("icon")).toString();
                if (entry.area.isEmpty()) entry.area = QStringLiteral("right");
                const QString visibility = attributes.value(QStringLiteral("visibility")).toString();
                entry.visible = visibility.compare(QStringLiteral("hidden"), Qt::CaseInsensitive) != 0 &&
                    visibility.compare(QStringLiteral("autohide"), Qt::CaseInsensitive) != 0;
                entry.floating = xmlBool(attributes.value(QStringLiteral("floating")).toString(), false);
                entry.floatable = xmlBool(attributes.value(QStringLiteral("floatable")).toString(), true);
                entry.preferredWidth = static_cast<int>(xmlNumber(
                    attributes.value(QStringLiteral("prefwidth")).toString(), 0));
                entry.preferredHeight = static_cast<int>(xmlNumber(
                    attributes.value(QStringLiteral("prefheight")).toString(), 0));
                for (const QXmlStreamAttribute& attribute : attributes)
                    entry.attributes.insert(attribute.name().toString(), attribute.value().toString());
                bool duplicateName = false;
                for (const DockLayoutEntry& existing : config.docks)
                    duplicateName = duplicateName || existing.name == name;
                if (!duplicateName)
                    config.docks.append(entry);
            } else if (tag == QStringLiteral("contentsview")) {
                ContentsViewLayoutEntry entry;
                entry.name = attributes.value(QStringLiteral("name")).toString();
                entry.text = attributes.value(QStringLiteral("text")).toString();
                entry.pluginId = attributes.value(QStringLiteral("pluginId")).toString();
                entry.factoryId = attributes.value(QStringLiteral("factoryId")).toString();
                entry.defaultHost = attributes.value(QStringLiteral("defaultHost")).toString();
                entry.icon = attributes.value(QStringLiteral("icon")).toString();
                for (const QXmlStreamAttribute& attribute : attributes)
                    entry.attributes.insert(attribute.name().toString(), attribute.value().toString());
                if (!entry.name.isEmpty()) {
                    config.contentsViews.append(entry);
                    if (warning && (entry.pluginId.isEmpty() || entry.factoryId.isEmpty())) {
                        *warning = QStringLiteral(
                            "内容视图缺少 pluginId/factoryId：%1").arg(entry.name);
                    }
                }
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == QStringLiteral("ribbonpages")) inRibbonPages = false;
            else if (xml.name() == QStringLiteral("rightbuttons")) inRightButtons = false;
            else if (xml.name() == QStringLiteral("ribbonpage")) {
                currentPageIndex = -1;
                currentGroupIndex = -1;
            } else if (xml.name() == QStringLiteral("ribbonpagegroup")) {
                currentGroupIndex = -1;
            }
        }
    }

    if (xml.hasError() && warning) {
        const QString backup = filePath + QStringLiteral(".corrupt-") +
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        QFile::copy(filePath, backup);
        *warning = QStringLiteral(
            "界面配置 XML 解析失败（第 %1 行）：%2；使用已读取值和内置默认值，已备份损坏文件到 %3")
            .arg(xml.lineNumber()).arg(xml.errorString()).arg(backup);
    }
    return config;
}

QString LayoutConfig::label(const QString& key, const QString& fallback) const
{
    return ribbonLabels.value(key, fallback);
}

} // namespace View3D

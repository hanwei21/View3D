#include "CrsBrowserWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace View3DCrs {

namespace {

QIcon uiIcon(const QString& name)
{
    static QHash<QString, QIcon> cache;
    const auto found = cache.constFind(name);
    if (found != cache.constEnd())
        return found.value();
    const QIcon icon(QDir(QCoreApplication::applicationDirPath())
                         .filePath(QStringLiteral("resources/icons/%1.svg").arg(name)));
    cache.insert(name, icon);
    return icon;
}

// 列表行的身份：kind 决定宿主右键菜单里哪些项可用，definition 是交给后端的文本，
// libraryId 只有「自定义」的行才有。
constexpr int kKindRole = Qt::UserRole;
constexpr int kDefinitionRole = Qt::UserRole + 1;
constexpr int kLibraryIdRole = Qt::UserRole + 2;
// 树节点带的是来源标记（fav / custom / builtin / 分组 id）。
constexpr int kNodeRole = Qt::UserRole;

constexpr const char* kFavKind = "fav";
constexpr const char* kCustomKind = "custom";
constexpr const char* kBuiltinKind = "builtin";

constexpr const char* kFavNode = "fav";
constexpr const char* kCustomNode = "custom";
constexpr const char* kBuiltinRoot = "builtin";

constexpr int kMaxListRows = 2000;

const QString kColumnSplit = QStringLiteral("　·　");

struct RowData {
    QString kind = QLatin1String(kBuiltinKind);
    QString definition;
    QString libraryId;
    QString name;
    QString tail;
    QString extra;
    QString haystack;
    bool geographic = false;
    bool projected = false;
    bool deprecated = false;
    bool favorite = false;
};

QString rowCaption(const RowData& row)
{
    QStringList tail;
    tail << (row.tail.isEmpty() ? QStringLiteral("自定义") : row.tail);
    if (row.favorite)
        tail << QStringLiteral("★");
    if (row.deprecated)
        tail << QStringLiteral("(废弃)");
    return QStringLiteral("%1　·　%2").arg(row.name, tail.join(kColumnSplit));
}

bool matches(const RowData& row, const QStringList& tokens)
{
    for (const QString& token : tokens) {
        if (!row.haystack.contains(token))
            return false;
    }
    return true;
}

void appendRowItem(QListWidget* list, const RowData& row)
{
    auto* item = new QListWidgetItem(list);
    item->setText(rowCaption(row));
    item->setData(kKindRole, row.kind);
    item->setData(kDefinitionRole, row.definition);
    item->setData(kLibraryIdRole, row.libraryId);
    if (!row.extra.isEmpty())
        item->setToolTip(row.extra);
}

// 详情页的配色跟着主题走：浅色下用浅灰卡片，深色下换成深灰，字色各自对得上。
struct DetailPalette {
    QString card;
    QString muted;
    QString chipBg;
    QString chipFg;
    QString codeBg;
    QString warnBg;
    QString warnFg;
};

DetailPalette paletteFor(const QWidget* widget)
{
    const bool dark = widget && widget->palette().color(QPalette::Window).lightness() < 128;
    DetailPalette colours;
    if (dark) {
        colours.card = QStringLiteral("#242a31");
        colours.muted = QStringLiteral("#9aa4ae");
        colours.chipBg = QStringLiteral("#243b55");
        colours.chipFg = QStringLiteral("#bcd6f5");
        colours.codeBg = QStringLiteral("#1b1f24");
        colours.warnBg = QStringLiteral("#4a2c2c");
        colours.warnFg = QStringLiteral("#f0b6b6");
    } else {
        colours.card = QStringLiteral("#f5f7fa");
        colours.muted = QStringLiteral("#6b7280");
        colours.chipBg = QStringLiteral("#e4eefb");
        colours.chipFg = QStringLiteral("#1c4e80");
        colours.codeBg = QStringLiteral("#f0f2f5");
        colours.warnBg = QStringLiteral("#fdecec");
        colours.warnFg = QStringLiteral("#a33a3a");
    }
    return colours;
}

QString chip(const QString& text, const QString& background, const QString& foreground)
{
    return QStringLiteral("<span style=\"background-color:%1; color:%2;\">&nbsp;%3&nbsp;</span>")
        .arg(background, foreground, text.toHtmlEscaped());
}

QString signedLongitude(double degrees)
{
    return QStringLiteral("%1 %2°")
        .arg(degrees < 0.0 ? QStringLiteral("西经") : QStringLiteral("东经"))
        .arg(QString::number(std::abs(degrees), 'f', 2));
}

QString signedLatitude(double degrees)
{
    return QStringLiteral("%1 %2°")
        .arg(degrees < 0.0 ? QStringLiteral("南纬") : QStringLiteral("北纬"))
        .arg(QString::number(std::abs(degrees), 'f', 2));
}

} // namespace

CrsBrowserWidget::CrsBrowserWidget(View3D::ICrsLibrary* library, View3D::ICrsBrowser* browser,
                                   QWidget* parent)
    : QWidget(parent), m_library(library), m_browser(browser)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);

    // ── 搜索与类型过滤 ─────────────────────────────────────────────────────
    auto* searchRow = new QHBoxLayout;
    searchRow->setSpacing(6);
    searchRow->addWidget(new QLabel(QStringLiteral("搜索"), this));
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("crs.manager.search"));
    m_search->setClearButtonEnabled(true);
    m_search->setPlaceholderText(QStringLiteral(
        "名称 / 权威码 / 中央经线 / 带号，例如 CGCS2000 114E、4547、zone 38"));
    searchRow->addWidget(m_search, 1);
    m_kindFilter = new QComboBox(this);
    m_kindFilter->setObjectName(QStringLiteral("crs.manager.kind"));
    m_kindFilter->addItems({ QStringLiteral("全部类型"), QStringLiteral("仅地理坐标系"),
                             QStringLiteral("仅投影坐标系") });
    searchRow->addWidget(m_kindFilter);
    root->addLayout(searchRow);

    // ── 左树 / 中列表 / 右详情 ─────────────────────────────────────────────
    auto* body = new QHBoxLayout;
    body->setSpacing(6);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("crs.manager.tree"));
    m_tree->setHeaderHidden(true);
    m_tree->setMinimumWidth(190);
    m_tree->setMaximumWidth(320);
    body->addWidget(m_tree, 1);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("crs.manager.list"));
    m_list->setAlternatingRowColors(true);
    m_list->setUniformItemSizes(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setMinimumWidth(220);
    body->addWidget(m_list, 2);

    auto* rightPanel = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(4);
    m_details = new QTextBrowser(rightPanel);
    m_details->setObjectName(QStringLiteral("crs.manager.details"));
    m_details->setReadOnly(true);
    m_details->setOpenExternalLinks(false);
    rightLayout->addWidget(m_details, 1);
    auto* copyRow = new QHBoxLayout;
    copyRow->setSpacing(4);
    m_copyProjButton = new QPushButton(QStringLiteral("复制 proj4"), rightPanel);
    m_copyProjButton->setObjectName(QStringLiteral("crs.manager.copy_proj"));
    m_copyProjButton->setIcon(uiIcon(QStringLiteral("action-copy")));
    m_copyWktButton = new QPushButton(QStringLiteral("复制 WKT2"), rightPanel);
    m_copyWktButton->setObjectName(QStringLiteral("crs.manager.copy_wkt"));
    m_copyWktButton->setIcon(uiIcon(QStringLiteral("action-copy")));
    copyRow->addWidget(m_copyProjButton);
    copyRow->addWidget(m_copyWktButton);
    copyRow->addStretch(1);
    rightLayout->addLayout(copyRow);
    body->addWidget(rightPanel, 3);
    root->addLayout(body, 1);

    connect(m_copyProjButton, &QPushButton::clicked, this, [this] {
        if (!m_detailProj.isEmpty())
            QApplication::clipboard()->setText(m_detailProj);
    });
    connect(m_copyWktButton, &QPushButton::clicked, this, [this] {
        if (!m_detailWkt.isEmpty())
            QApplication::clipboard()->setText(m_detailWkt);
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) {
        showDetails();
        if (m_onSelectionChanged)
            m_onSelectionChanged();
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        if (m_onActivated && hasSelection())
            m_onActivated();
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        m_contextPos = pos;
        if (m_onContextMenu)
            m_onContextMenu(pos);
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                if (!current)
                    return;
                m_builtinGroupId = current->data(0, kNodeRole).toString();
                populateList();
            });
    connect(m_search, &QLineEdit::textChanged, this, [this] { populateList(); });
    connect(m_kindFilter, &QComboBox::currentIndexChanged, this, [this](int) { populateList(); });
}

void CrsBrowserWidget::refresh()
{
    if (!m_library)
        return;
    rebuildTree();
    if (m_tree && !m_tree->currentItem())
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
    populateList();
}

void CrsBrowserWidget::rebuildTree()
{
    if (!m_tree || !m_library)
        return;
    const QString selected =
        m_tree->currentItem() ? m_tree->currentItem()->data(0, kNodeRole).toString() : QString();
    {
        const QSignalBlocker blocker(m_tree);
        m_tree->clear();

        auto* favourites = new QTreeWidgetItem(m_tree);
        favourites->setText(0, QStringLiteral("常用（%1）").arg(m_library->favorites().size()));
        favourites->setIcon(0, uiIcon(QStringLiteral("action-favorite-on")));
        favourites->setData(0, kNodeRole, QLatin1String(kFavNode));
        favourites->setToolTip(0, QStringLiteral("显示参照系下拉里就是这些，右键可取消收藏"));

        auto* custom = new QTreeWidgetItem(m_tree);
        custom->setText(0, QStringLiteral("自定义（%1）").arg(int(m_library->entries().size())));
        custom->setIcon(0, uiIcon(QStringLiteral("action-save-custom")));
        custom->setData(0, kNodeRole, QLatin1String(kCustomNode));
        custom->setToolTip(0, QStringLiteral("新建 / 导入的参照系，可编辑、可删除"));

        auto* builtin = new QTreeWidgetItem(m_tree);
        builtin->setIcon(0, uiIcon(QStringLiteral("crs-grid")));
        builtin->setData(0, kNodeRole, QLatin1String(kBuiltinRoot));
        if (m_browser) {
            const QList<View3D::ICrsBrowser::Group>& groups = m_browser->groups();
            int total = 0;
            for (const View3D::ICrsBrowser::Group& group : groups)
                total += int(group.items.size());
            builtin->setText(0, QStringLiteral("EPSG 内置（%1）").arg(total));
            builtin->setToolTip(0, QStringLiteral("只读：右键可收藏或存进自定义"));
            for (const View3D::ICrsBrowser::Group& group : groups) {
                auto* child = new QTreeWidgetItem(builtin);
                child->setText(0, QStringLiteral("%1（%2）")
                                      .arg(group.label, QString::number(group.items.size())));
                child->setData(0, kNodeRole, group.id);
            }
            builtin->setExpanded(true);
        } else {
            builtin->setText(0, QStringLiteral("EPSG 内置（不可用）"));
            builtin->setToolTip(0, QStringLiteral("PROJ 权威库读不出来"));
        }
    }
    // 恢复原来选中的分支：库一变（新建、收藏）不至于把用户弹回顶上。
    const auto restore = [&](const auto& self, QTreeWidgetItem* node) -> bool {
        if (!node)
            return false;
        if (node->data(0, kNodeRole).toString() == selected) {
            m_tree->setCurrentItem(node);
            return true;
        }
        for (int index = 0; index < node->childCount(); ++index) {
            if (self(self, node->child(index)))
                return true;
        }
        return false;
    };
    for (int index = 0; index < m_tree->topLevelItemCount(); ++index) {
        if (restore(restore, m_tree->topLevelItem(index)))
            break;
    }
}

CrsBrowserWidget::Source CrsBrowserWidget::currentSource() const
{
    QTreeWidgetItem* node = m_tree ? m_tree->currentItem() : nullptr;
    const QString token = node ? node->data(0, kNodeRole).toString() : QString();
    if (token == QLatin1String(kFavNode))
        return Source::Favourites;
    if (token == QLatin1String(kCustomNode))
        return Source::Custom;
    return Source::Builtin;
}

void CrsBrowserWidget::populateList()
{
    if (!m_list || !m_library)
        return;
    m_hint.clear();
    const QStringList tokens =
        m_search->text().simplified().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const int kindIndex = m_kindFilter ? m_kindFilter->currentIndex() : 0;

    QList<RowData> rows;
    const auto accept = [&](RowData&& row) {
        if (kindIndex == 1 && !row.geographic)
            return;
        if (kindIndex == 2 && !row.projected)
            return;
        if (!matches(row, tokens))
            return;
        rows.append(std::move(row));
    };
    const auto customRow = [&](const View3D::CrsLibraryEntry& entry) {
        RowData row;
        row.kind = QLatin1String(kCustomKind);
        row.definition = entry.effectiveDefinition();
        row.libraryId = entry.id;
        row.name = entry.name;
        row.tail = entry.code.isEmpty() ? entry.kindLabel() : entry.code;
        row.extra = entry.projString;
        row.geographic = entry.geographic;
        row.projected = entry.projected;
        row.haystack = QStringLiteral("%1 %2 %3 %4 %5")
                           .arg(entry.name, entry.code, entry.definition, entry.projString,
                                entry.note)
                           .toLower();
        row.favorite =
            m_library->isFavorite(row.definition) || m_library->isFavorite(entry.definition);
        return row;
    };
    const auto builtinRow = [&](const View3D::ICrsBrowser::Item& item) {
        RowData row;
        row.kind = QLatin1String(kBuiltinKind);
        row.definition = item.key;
        row.name = item.name;
        row.tail = item.key;
        row.extra = item.method;
        row.geographic = item.geographic;
        row.projected = item.projected;
        row.deprecated = item.deprecated;
        row.haystack = QStringLiteral("%1 %2 %3 %4 %5")
                           .arg(item.name, item.key, item.code, item.method, item.areaName)
                           .toLower();
        row.favorite = m_library->isFavorite(item.key);
        return row;
    };

    const Source source = currentSource();
    if (source == Source::Custom) {
        for (const View3D::CrsLibraryEntry& entry : m_library->entries())
            accept(customRow(entry));
    } else if (source == Source::Favourites) {
        for (const QString& definition : m_library->favorites()) {
            // 收藏按定义回指：自定义里有同一条就用它（能改能删），否则回指内置。
            const View3D::CrsLibraryEntry* stored = nullptr;
            for (const View3D::CrsLibraryEntry& entry : m_library->entries()) {
                if (entry.effectiveDefinition() == definition || entry.definition == definition) {
                    stored = &entry;
                    break;
                }
            }
            View3D::ICrsBrowser::Item item;
            if (stored) {
                accept(customRow(*stored));
            } else if (m_browser && m_browser->item(definition, &item)) {
                accept(builtinRow(item));
            } else {
                RowData row;
                row.kind = QLatin1String(kFavKind);
                row.definition = definition;
                row.name = definition;
                row.tail = QStringLiteral("常用");
                row.haystack = definition.toLower();
                row.favorite = true;
                accept(std::move(row));
            }
        }
    } else if (m_browser) {
        // 没搜索时只列选中的分组；一开始搜索就跨全部分组找（内置根节点等于全列）。
        const bool allGroups = !tokens.isEmpty() ||
            m_builtinGroupId == QLatin1String(kBuiltinRoot) || m_builtinGroupId.isEmpty();
        for (const View3D::ICrsBrowser::Group& group : m_browser->groups()) {
            if (!allGroups && group.id != m_builtinGroupId)
                continue;
            for (const View3D::ICrsBrowser::Item& item : group.items)
                accept(builtinRow(item));
            if (rows.size() > kMaxListRows)
                break;
        }
    }

    const int total = int(rows.size());
    if (rows.size() > kMaxListRows)
        rows = rows.mid(0, kMaxListRows);

    const QString keepDefinition = currentDefinition();
    {
        const QSignalBlocker blocker(m_list);
        m_list->clear();
        for (const RowData& row : std::as_const(rows))
            appendRowItem(m_list, row);
        int select = -1;
        for (int index = 0; index < m_list->count(); ++index) {
            if (m_list->item(index)->data(kDefinitionRole).toString() == keepDefinition) {
                select = index;
                break;
            }
        }
        if (select < 0 && m_list->count() > 0)
            select = 0;
        if (select >= 0)
            m_list->setCurrentRow(select);
    }
    showDetails();
    if (m_onSelectionChanged)
        m_onSelectionChanged();
    if (total > kMaxListRows) {
        m_hint = QStringLiteral("结果 %1 条太多，只列前 %2 条：继续输入缩小范围")
                     .arg(total)
                     .arg(kMaxListRows);
    } else if (total == 0 && !tokens.isEmpty()) {
        m_hint = QStringLiteral("没搜到「%1」：试试只输带号或中央经线").arg(m_search->text());
    }
}

QString CrsBrowserWidget::currentDefinition() const
{
    if (!m_list || m_list->currentRow() < 0)
        return {};
    return m_list->item(m_list->currentRow())->data(kDefinitionRole).toString();
}

QString CrsBrowserWidget::currentLibraryId() const
{
    if (!m_list || m_list->currentRow() < 0)
        return {};
    return m_list->item(m_list->currentRow())->data(kLibraryIdRole).toString();
}

bool CrsBrowserWidget::currentIsCustom() const
{
    if (!m_list || m_list->currentRow() < 0)
        return false;
    return m_list->item(m_list->currentRow())->data(kKindRole).toString() ==
        QLatin1String(kCustomKind);
}

bool CrsBrowserWidget::currentIsBuiltin() const
{
    return hasSelection() && !currentIsCustom();
}

bool CrsBrowserWidget::hasSelection() const
{
    return m_list && m_list->currentRow() >= 0;
}

void CrsBrowserWidget::selectCustomBranch()
{
    if (!m_tree)
        return;
    for (int index = 0; index < m_tree->topLevelItemCount(); ++index) {
        QTreeWidgetItem* node = m_tree->topLevelItem(index);
        if (node->data(0, kNodeRole).toString() == QLatin1String(kCustomNode)) {
            // 设当前节点就会触发 populateList，这里不再手动补一次。
            m_tree->setCurrentItem(node);
            return;
        }
    }
}

void CrsBrowserWidget::selectDefinition(const QString& definition)
{
    if (!m_list)
        return;
    for (int index = 0; index < m_list->count(); ++index) {
        if (m_list->item(index)->data(kDefinitionRole).toString() == definition) {
            m_list->setCurrentRow(index);
            return;
        }
    }
    const QSignalBlocker blocker(m_list);
    m_list->clearSelection();
    m_list->setCurrentItem(nullptr);
    showDetails();
    if (m_onSelectionChanged)
        m_onSelectionChanged();
}

void CrsBrowserWidget::showDetails()
{
    if (!m_details)
        return;
    m_detailProj.clear();
    m_detailWkt.clear();
    const QString definition = currentDefinition();
    const DetailPalette colours = paletteFor(this);
    if (definition.isEmpty()) {
        m_details->setHtml(
            QStringLiteral("<div style=\"color:%1\">左边选一个来源，或在上边搜索。<br><br>"
                           "内置参照系来自 PROJ 自带的权威库 proj.db（和 QGIS 同一份数据），"
                           "只读：右键「收藏」进常用，右键「存进自定义」复制一条再改。</div>")
                .arg(colours.muted));
    } else if (!m_browser) {
        m_details->setPlainText(QStringLiteral("内置参照系服务不可用，只看得到自定义库：\n%1")
                                    .arg(definition));
    } else {
        View3D::ICrsBrowser::Info info;
        if (!m_browser->info(definition, &info)) {
            m_details->setHtml(
                QStringLiteral("<table width=\"100%\" cellpadding=\"8\" bgcolor=\"%1\">"
                               "<tr><td style=\"color:%2\">！%3</td></tr></table>")
                    .arg(colours.warnBg, colours.warnFg,
                         m_browser->lastError().toHtmlEscaped()));
        } else {
            QStringList chips;
            if (!info.code.isEmpty())
                chips << chip(info.code, colours.chipBg, colours.chipFg);
            if (!info.kind.isEmpty())
                chips << chip(info.kind, colours.chipBg, colours.chipFg);
            if (info.deprecated)
                chips << chip(QStringLiteral("已废弃"), colours.warnBg, colours.warnFg);

            QStringList cardRows;
            const auto addRow = [&cardRows, &colours](const QString& key, const QString& value) {
                if (value.trimmed().isEmpty())
                    return;
                cardRows << QStringLiteral(
                                "<tr><td width=\"84\" style=\"color:%1\">%2</td><td>%3</td></tr>")
                                .arg(colours.muted, key, value.toHtmlEscaped());
            };
            addRow(QStringLiteral("投影方法"), info.method);
            addRow(QStringLiteral("基准"), info.datum);
            addRow(QStringLiteral("椭球"), info.ellipsoid);
            addRow(QStringLiteral("状态"),
                   info.deprecated ? QStringLiteral("已废弃（旧成果会用到）")
                                   : QStringLiteral("现行"));
            addRow(QStringLiteral("用途"), info.scope);
            addRow(QStringLiteral("备注"), info.remarks);

            QString html;
            html += QStringLiteral("<div style=\"font-size:17pt; font-weight:600;\">%1</div>")
                        .arg((info.name.isEmpty() ? QStringLiteral("(没有名字)") : info.name)
                                 .toHtmlEscaped());
            if (!chips.isEmpty())
                html += QStringLiteral("<div style=\"margin:6px 0 10px 0;\">%1</div>")
                            .arg(chips.join(QStringLiteral("&nbsp;")));
            if (!cardRows.isEmpty()) {
                html += QStringLiteral(
                            "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"6\" "
                            "bgcolor=\"%1\"><tr><td colspan=\"2\"><b>基本</b></td></tr>%2</table>")
                            .arg(colours.card, cardRows.join(QString()));
            }
            if (info.hasArea) {
                html += QStringLiteral(
                            "<p style=\"margin:12px 0 2px 0;\"><b>使用范围</b></p>"
                            "<div>%1</div><div style=\"color:%2;\">%3 ~ %4，%5 ~ %6</div>")
                            .arg(info.areaName.toHtmlEscaped(), colours.muted,
                                 signedLongitude(info.west), signedLongitude(info.east),
                                 signedLatitude(info.south), signedLatitude(info.north));
            }
            if (!info.projString.isEmpty()) {
                html += QStringLiteral("<p style=\"margin:12px 0 2px 0;\"><b>proj4</b></p>"
                                       "<pre style=\"background-color:%1;\">%2</pre>")
                            .arg(colours.codeBg, info.projString.toHtmlEscaped());
            }
            if (!info.wkt.isEmpty()) {
                html += QStringLiteral("<p style=\"margin:12px 0 2px 0;\"><b>WKT2</b></p>"
                                       "<pre style=\"background-color:%1;\">%2</pre>")
                            .arg(colours.codeBg, info.wkt.toHtmlEscaped());
            } else {
                html += QStringLiteral(
                            "<p style=\"color:%1;\">WKT2：PROJ 导不出完整坐标系，用上面的 proj4 文本</p>")
                            .arg(colours.muted);
            }
            m_details->setHtml(html);
            m_detailProj = info.projString;
            m_detailWkt = info.wkt;
        }
    }
    if (m_copyProjButton)
        m_copyProjButton->setEnabled(!m_detailProj.isEmpty());
    if (m_copyWktButton)
        m_copyWktButton->setEnabled(!m_detailWkt.isEmpty());
}

void CrsBrowserWidget::setOnSelectionChanged(std::function<void()> callback)
{
    m_onSelectionChanged = std::move(callback);
}

void CrsBrowserWidget::setOnContextMenuRequested(std::function<void(const QPoint&)> callback)
{
    m_onContextMenu = std::move(callback);
}

void CrsBrowserWidget::setOnActivated(std::function<void()> callback)
{
    m_onActivated = std::move(callback);
}

} // namespace View3DCrs

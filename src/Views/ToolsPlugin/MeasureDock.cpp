#include "MeasureDock.h"

#include "MeasureSession.h"

#include <QAbstractItemView>
#include <QAction>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace View3DPlugins {
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

} // namespace

MeasureDock::MeasureDock(QString name, QString caption)
    : m_name(std::move(name)), m_caption(std::move(caption))
{
    if (m_name.isEmpty())
        m_name = QStringLiteral("dw.measurements");
    if (m_caption.isEmpty())
        m_caption = QStringLiteral("测量结果");

    m_widget = new QWidget();
    auto* layout = new QVBoxLayout(m_widget);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    m_hint = new QLabel(m_widget);
    m_hint->setObjectName(QStringLiteral("measure.hint"));
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    m_table = new QTableWidget(m_widget);
    m_table->setObjectName(QStringLiteral("measure.table"));
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("类型"), QStringLiteral("结果"), QStringLiteral("坐标 / 顶点")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideRight);
    m_table->setMinimumWidth(240);
    m_table->verticalHeader()->setVisible(false);
    // 坐标/顶点文本很长，按内容扩列会把整个 Dock 的 sizeHint 撑到上千像素；
    // 改用弹性列 + 省略号，完整内容放到 tooltip 里。
    auto* header = m_table->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_table, 1);

    QObject::connect(m_table, &QTableWidget::cellClicked, m_table,
                     [this](int row, int) {
                         if (!m_updating && m_session)
                             m_session->displayResult(row, true);
                     });
    QObject::connect(m_table, &QTableWidget::cellDoubleClicked, m_table,
                     [this](int row, int) {
                         if (!m_updating && m_session)
                             m_session->locateResult(row);
                     });
    QObject::connect(m_table, &QTableWidget::customContextMenuRequested, m_table,
                     [this](const QPoint& position) { showContextMenu(position); });
}

void MeasureDock::setSession(MeasureSession* session)
{
    m_session = session;
    if (!m_session)
        setRows({}, -1);
}

void MeasureDock::setHint(const QString& text)
{
    if (m_hint)
        m_hint->setText(text);
}

void MeasureDock::setRows(const QList<QStringList>& rows, int displayedRow)
{
    if (!m_table)
        return;
    m_updating = true;
    m_table->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const QStringList& values = rows.at(row);
        for (int column = 0; column < m_table->columnCount(); ++column) {
            const QString text = column < values.size() ? values.at(column) : QString();
            auto* item = new QTableWidgetItem(text);
            item->setToolTip(text);
            if (row == displayedRow) {
                QFont font = item->font();
                font.setBold(true);
                item->setFont(font);
                item->setBackground(QColor(232, 240, 254));
            }
            m_table->setItem(row, column, item);
        }
    }
    if (displayedRow >= 0 && displayedRow < rows.size())
        m_table->selectRow(displayedRow);
    else
        m_table->clearSelection();
    m_updating = false;
}

void MeasureDock::showContextMenu(const QPoint& position)
{
    if (!m_session || !m_table)
        return;
    const int row = m_table->rowAt(position.y());
    const QList<MeasureResult>& results = m_session->results();
    if (row < 0 || row >= results.size()) {
        if (results.isEmpty())
            return;
        QMenu menu(m_table);
        QAction* clear = menu.addAction(QStringLiteral("清空全部结果"));
        clear->setIcon(uiIcon(QStringLiteral("action-clear")));
        if (menu.exec(m_table->viewport()->mapToGlobal(position)) == clear)
            m_session->clearResults();
        return;
    }

    QMenu menu(m_table);
    QAction* locate = menu.addAction(QStringLiteral("定位到测量"));
    locate->setIcon(uiIcon(QStringLiteral("action-locate")));
    const bool displayed = m_session->displayedRow() == row;
    QAction* toggle = menu.addAction(displayed ? QStringLiteral("关闭该测量显示")
                                               : QStringLiteral("只显示该测量"));
    toggle->setIcon(uiIcon(displayed ? QStringLiteral("action-hide")
                                     : QStringLiteral("action-show")));
    QAction* remove = menu.addAction(QStringLiteral("删除测量"));
    remove->setIcon(uiIcon(QStringLiteral("action-delete")));
    menu.addSeparator();
    QAction* clear = menu.addAction(QStringLiteral("清空全部结果"));
    clear->setIcon(uiIcon(QStringLiteral("action-clear")));
    QAction* chosen = menu.exec(m_table->viewport()->mapToGlobal(position));
    if (!chosen)
        return;
    if (chosen == locate)
        m_session->locateResult(row);
    else if (chosen == toggle)
        m_session->displayResult(row, true);
    else if (chosen == remove)
        m_session->removeResult(row);
    else if (chosen == clear)
        m_session->clearResults();
}

} // namespace View3DPlugins

#include "FeatureTableDock.h"

#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

#include <utility>

namespace View3DFeature {

FeatureTableDock::FeatureTableDock(QString name, QString caption)
    : m_name(std::move(name)), m_caption(std::move(caption))
{
    m_widget = new QWidget();
    auto* layout = new QVBoxLayout(m_widget);
    layout->setContentsMargins(6, 6, 6, 6);
    m_title = new QLabel(QStringLiteral("未选择要素"), m_widget);
    m_title->setObjectName(QStringLiteral("feature.title"));
    m_title->setWordWrap(true);
    layout->addWidget(m_title);

    m_table = new QTableWidget(0, 2, m_widget);
    m_table->setObjectName(QStringLiteral("feature.table"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("字段"), QStringLiteral("值")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_table);
}

void FeatureTableDock::showFeature(const FeatureSelection& selection)
{
    QString title = QStringLiteral("要素 #%1").arg(selection.featureId);
    if (selection.tableFeatureCountValid && selection.tableFeatureCount > 0)
        title += QStringLiteral("（本表共 %1 个要素）").arg(selection.tableFeatureCount);
    if (!selection.layerName.isEmpty())
        title += QStringLiteral("  ·  图层 %1").arg(selection.layerName);
    m_title->setText(title);
    // 完整渲染键放进提示，排查"点到哪块瓦片/哪个图层"时能直接拷走。
    m_title->setToolTip(selection.sourceKey);

    const QVariantMap& attributes = selection.attributes;
    m_table->setRowCount(attributes.size());
    int row = 0;
    for (auto it = attributes.constBegin(); it != attributes.constEnd(); ++it) {
        m_table->setItem(row, 0, new QTableWidgetItem(it.key()));
        m_table->setItem(row, 1, new QTableWidgetItem(it.value().toString()));
        ++row;
    }
}

void FeatureTableDock::clearSelection()
{
    m_title->setText(QStringLiteral("未选择要素"));
    m_title->setToolTip(QString());
    m_table->setRowCount(0);
}

} // namespace View3DFeature

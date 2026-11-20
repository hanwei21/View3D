#include "CrsSelectDialog.h"

#include "CrsBrowserWidget.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace View3DCrs {

CrsSelectDialog::CrsSelectDialog(View3D::ICrsLibrary* library, View3D::ICrsBrowser* browser,
                                 QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("crs.select.dialog"));
    setWindowTitle(QStringLiteral("选择显示参照系"));
    setModal(true);
    resize(920, 560);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(6);
    root->addWidget(new QLabel(
        QStringLiteral("选一条参照系作为场景的显示参照系（局部轴按它的网格北取向）。"
                       "双击列表项也可以直接确定。"),
        this));
    m_browser = new CrsBrowserWidget(library, browser, this);
    root->addWidget(m_browser, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setObjectName(QStringLiteral("crs.select.ok"));
    m_ok->setText(QStringLiteral("确定"));
    auto* cancel = buttons->button(QDialogButtonBox::Cancel);
    cancel->setObjectName(QStringLiteral("crs.select.cancel"));
    cancel->setText(QStringLiteral("取消"));
    root->addWidget(buttons);

    m_browser->setOnSelectionChanged([this] {
        if (m_ok)
            m_ok->setEnabled(m_browser->hasSelection());
    });
    m_browser->setOnActivated([this] {
        if (m_browser->hasSelection())
            accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_browser->hasSelection())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_browser->refresh();
    m_ok->setEnabled(m_browser->hasSelection());
}

void CrsSelectDialog::preselect(const QString& definition)
{
    if (!m_browser || definition.isEmpty())
        return;
    m_browser->selectDefinition(definition);
    if (m_ok)
        m_ok->setEnabled(m_browser->hasSelection());
}

QString CrsSelectDialog::selectedDefinition() const
{
    return m_browser ? m_browser->currentDefinition() : QString();
}

} // namespace View3DCrs

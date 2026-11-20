#include "CrsEditorDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

namespace View3DCrs {

CrsEditorDialog::CrsEditorDialog(const View3D::ICrsBrowser* browser, QWidget* parent)
    : QDialog(parent), m_browser(browser)
{
    setObjectName(QStringLiteral("crs.editor.dialog"));
    setWindowTitle(QStringLiteral("自定义参照系"));
    setModal(true);
    resize(520, 460);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(6);

    root->addWidget(new QLabel(QStringLiteral("名称（留空则用 PROJ 解析出的官方名）"), this));
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("crs.editor.name"));
    root->addWidget(m_name);

    root->addWidget(new QLabel(QStringLiteral("定义（EPSG 码 / proj 字符串 / WKT）"), this));
    m_definition = new QPlainTextEdit(this);
    m_definition->setObjectName(QStringLiteral("crs.editor.definition"));
    m_definition->setPlaceholderText(
        QStringLiteral("EPSG:4547\n+proj=tmerc +lat_0=0 +lon_0=114 +k_0=0.9996 "
                       "+x_0=500000 +y_0=0 +ellps=GRS80 +units=m +no_defs\nPROJCS[\"…\"]"));
    m_definition->setMinimumHeight(150);
    root->addWidget(m_definition, 1);

    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("crs.editor.preview"));
    m_preview->setWordWrap(true);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview->setFrameShape(QFrame::StyledPanel);
    m_preview->setMargin(6);
    root->addWidget(m_preview);

    root->addWidget(new QLabel(QStringLiteral("备注"), this));
    m_note = new QLineEdit(this);
    m_note->setObjectName(QStringLiteral("crs.editor.note"));
    root->addWidget(m_note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setObjectName(QStringLiteral("crs.editor.ok"));
    m_ok->setText(QStringLiteral("确定"));
    m_ok->setEnabled(false);
    auto* cancel = buttons->button(QDialogButtonBox::Cancel);
    cancel->setObjectName(QStringLiteral("crs.editor.cancel"));
    cancel->setText(QStringLiteral("取消"));
    root->addWidget(buttons);

    // 预览带 120 ms 防抖，但「确定」的可否用同步算：测试与用户都不会卡在
    // 「刚粘完定义、按钮还是灰的」这个中间态上。
    auto* previewTimer = new QTimer(this);
    previewTimer->setSingleShot(true);
    previewTimer->setInterval(120);
    connect(previewTimer, &QTimer::timeout, this, [this] { refreshPreview(); });

    const auto revalidate = [this, previewTimer] {
        QString reason;
        const bool valid = validate(&reason);
        m_ok->setEnabled(valid);
        if (!valid)
            m_preview->setText(reason.isEmpty() ? QStringLiteral("先填一条参照系定义")
                                                : QStringLiteral("！%1").arg(reason));
        previewTimer->start();
    };
    connect(m_definition, &QPlainTextEdit::textChanged, this, revalidate);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QString reason;
        if (!validate(&reason)) {
            m_preview->setText(QStringLiteral("！%1").arg(reason));
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    revalidate();
}

void CrsEditorDialog::setEntry(const QString& name, const QString& definition, const QString& note)
{
    m_name->setText(name);
    m_definition->setPlainText(definition);
    m_note->setText(note);
    refreshPreview();
}

QString CrsEditorDialog::entryName() const
{
    return m_name->text().trimmed();
}

QString CrsEditorDialog::definition() const
{
    return m_definition->toPlainText().trimmed();
}

QString CrsEditorDialog::note() const
{
    return m_note->text().trimmed();
}

bool CrsEditorDialog::validate(QString* reason) const
{
    const QString definition = m_definition->toPlainText().trimmed();
    if (definition.isEmpty()) {
        if (reason)
            *reason = QStringLiteral("定义不能为空");
        return false;
    }
    if (!m_browser) {
        if (reason)
            *reason = QStringLiteral("没有可用的参照系服务，先打开一次参照系面板");
        return false;
    }
    View3D::ICrsBrowser::Info info;
    if (!m_browser->info(definition, &info)) {
        if (reason)
            *reason = m_browser->lastError();
        return false;
    }
    return true;
}

void CrsEditorDialog::refreshPreview()
{
    const QString definition = m_definition->toPlainText().trimmed();
    if (definition.isEmpty()) {
        m_preview->setText(QStringLiteral("把 EPSG 码、proj 字符串或 WKT 粘进来，这里会显示解析结果"));
        return;
    }
    if (!m_browser) {
        m_preview->setText(QStringLiteral("参照系服务不可用，无法预览"));
        return;
    }
    View3D::ICrsBrowser::Info info;
    if (!m_browser->info(definition, &info)) {
        m_preview->setText(QStringLiteral("！%1").arg(m_browser->lastError()));
        return;
    }
    QStringList lines;
    lines << QStringLiteral("名称：%1")
                 .arg(info.name.isEmpty() ? QStringLiteral("(没有名字)") : info.name);
    lines << QStringLiteral("权威码：%1　　类型：%2")
                 .arg(info.code.isEmpty() ? QStringLiteral("(自定义定义，无权威码)") : info.code,
                      info.kind);
    if (!info.method.isEmpty())
        lines << QStringLiteral("投影方法：%1").arg(info.method);
    if (info.hasArea) {
        lines << QStringLiteral("使用范围：%1")
                     .arg(info.areaName.isEmpty() ? QStringLiteral("(未记名)") : info.areaName);
    }
    m_preview->setText(lines.join(QLatin1Char('\n')));
}

} // namespace View3DCrs

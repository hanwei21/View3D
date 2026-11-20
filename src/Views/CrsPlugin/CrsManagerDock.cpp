#include "CrsManagerDock.h"

#include "CrsBrowserWidget.h"
#include "CrsEditorDialog.h"

#include <IApplication.h>
#include <IGeoReference.h>
#include <IMessageCenter.h>
#include <IPluginContainer.h>
#include <View3DObjectIds.h>

#include <QAction>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

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

} // namespace

CrsManagerDock::CrsManagerDock(QString name, QString caption)
    : m_name(std::move(name)), m_caption(std::move(caption))
{
    buildUi();
}

void CrsManagerDock::buildUi()
{
    m_widget = new QWidget;
    m_widget->setObjectName(m_name);
    auto* root = new QVBoxLayout(m_widget);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // ── 工具栏：改库的动作都自动落盘，所以没有「写入库文件」这一步 ───────────
    auto* actions = new QHBoxLayout;
    actions->setSpacing(4);
    m_newButton = new QPushButton(QStringLiteral("新建…"), m_widget);
    m_newButton->setObjectName(QStringLiteral("crs.manager.new"));
    m_newButton->setIcon(uiIcon(QStringLiteral("action-new")));
    m_newButton->setToolTip(QStringLiteral("打开对话框，写一条自定义参照系定义"));
    m_newButton->setEnabled(false);
    m_importButton = new QPushButton(QStringLiteral("导入…"), m_widget);
    m_importButton->setObjectName(QStringLiteral("crs.manager.import"));
    m_importButton->setIcon(uiIcon(QStringLiteral("action-import")));
    m_importButton->setToolTip(QStringLiteral("导入 .prj / .wkt / .json 参照系定义"));
    m_importButton->setEnabled(false);
    m_exportButton = new QPushButton(QStringLiteral("导出…"), m_widget);
    m_exportButton->setObjectName(QStringLiteral("crs.manager.export"));
    m_exportButton->setIcon(uiIcon(QStringLiteral("action-export")));
    m_exportButton->setToolTip(QStringLiteral("导出选中的自定义条目，或整库（JSON / .prj / .wkt）"));
    m_exportButton->setEnabled(false);
    actions->addWidget(m_newButton);
    actions->addWidget(m_importButton);
    actions->addWidget(m_exportButton);
    actions->addStretch(1);
    root->addLayout(actions);

    // 右键菜单的动作挂在面板上，菜单只是临时容器：无头用例能直接按 objectName 取到。
    m_favoriteAction = new QAction(QStringLiteral("收藏"), m_widget);
    m_favoriteAction->setObjectName(QStringLiteral("crs.action.favorite"));
    m_favoriteAction->setIcon(uiIcon(QStringLiteral("action-favorite")));
    m_editAction = new QAction(QStringLiteral("编辑…"), m_widget);
    m_editAction->setObjectName(QStringLiteral("crs.action.edit"));
    m_editAction->setIcon(uiIcon(QStringLiteral("action-edit")));
    m_promoteAction = new QAction(QStringLiteral("存进自定义…"), m_widget);
    m_promoteAction->setObjectName(QStringLiteral("crs.action.promote"));
    m_promoteAction->setIcon(uiIcon(QStringLiteral("action-save-custom")));
    m_deleteAction = new QAction(QStringLiteral("删除"), m_widget);
    m_deleteAction->setObjectName(QStringLiteral("crs.action.delete"));
    m_deleteAction->setIcon(uiIcon(QStringLiteral("action-delete")));

    m_status = new QLabel(QStringLiteral("等待参照系服务…"), m_widget);
    m_status->setObjectName(QStringLiteral("crs.manager.status"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_status);

    QObject::connect(m_newButton, &QPushButton::clicked, m_widget, [this] { startNewEntry(); });
    QObject::connect(m_importButton, &QPushButton::clicked, m_widget, [this] { importLibrary(); });
    QObject::connect(m_exportButton, &QPushButton::clicked, m_widget, [this] { exportLibrary(); });
    QObject::connect(m_favoriteAction, &QAction::triggered, m_widget, [this] { toggleFavorite(); });
    QObject::connect(m_editAction, &QAction::triggered, m_widget, [this] { editCurrent(); });
    QObject::connect(m_promoteAction, &QAction::triggered, m_widget,
                     [this] { promoteToCustom(); });
    QObject::connect(m_deleteAction, &QAction::triggered, m_widget, [this] { removeCurrent(); });
}

void CrsManagerDock::onCreate(RF::PluginEngine::IApplication* app)
{
    m_application = app;
    m_container = app ? app->getPluginContainer() : nullptr;
    // 库与内置列表可能比面板先就绪，也可能后到：两头都试一次，后到的那头发广播补绑定。
    if (m_container && app) {
        m_subscription = app->getMessageCenter()->subscribe(
            View3D::MessageTopics::CrsLibraryReady, this,
            [this](RF::PluginEngine::Message&) { refreshBindings(); });
    }
    refreshBindings();
}

void CrsManagerDock::onDestroy()
{
    if (m_application && m_subscription != 0)
        m_application->getMessageCenter()->unsubscribe(m_subscription);
    m_subscription = 0;
    m_library = nullptr;
    m_reference = nullptr;
    m_browser = nullptr;
    m_application = nullptr;
    m_container = nullptr;
}

void CrsManagerDock::onActive(bool isActive)
{
    if (isActive)
        refreshBindings();
}

void CrsManagerDock::refreshBindings()
{
    m_library = m_container
        ? m_container->objectAs<View3D::ICrsLibrary>(View3D::ServiceIds::CrsLibrary)
        : nullptr;
    m_reference = m_container
        ? m_container->objectAs<View3D::IGeoReference>(View3D::ServiceIds::GeoReference)
        : nullptr;
    m_browser = m_container
        ? m_container->objectAs<View3D::ICrsBrowser>(View3D::ServiceIds::CrsBrowser)
        : nullptr;
    const bool bound = m_library != nullptr;
    for (QPushButton* button : { m_newButton, m_importButton, m_exportButton }) {
        if (button)
            button->setEnabled(bound);
    }
    if (!bound) {
        setStatus(QStringLiteral("等待参照系服务（plugin.view3d.crs 未加载？）"), true);
        return;
    }
    buildBrowser();
    m_browserWidget->refresh();
    QString text = QStringLiteral("自定义 %1 条，常用 %2 条；库文件：%3")
                       .arg(int(m_library->entries().size()))
                       .arg(m_library->favorites().size())
                       .arg(m_library->filePath());
    if (m_browser) {
        // 第一次访问才真的枚举权威库（本机实测 6215 条约 80 ms），所以放在绑定之后。
        m_browser->groups();
        text += QStringLiteral("；%1").arg(m_browser->statusText());
    } else {
        text += QStringLiteral("；内置参照系服务不可用");
    }
    setStatus(text);
    updateActions();
}

void CrsManagerDock::buildBrowser()
{
    if (m_browserWidget || !m_library)
        return;
    m_browserWidget = new CrsBrowserWidget(m_library, m_browser, m_widget);
    // 控件放在状态行上面（状态行在 buildUi 里已经加进去了，插到它前面）。
    if (auto* layout = qobject_cast<QVBoxLayout*>(m_widget->layout())) {
        const int statusIndex = layout->indexOf(m_status);
        layout->insertWidget(statusIndex >= 0 ? statusIndex : layout->count(), m_browserWidget, 1);
    }
    m_browserWidget->setOnSelectionChanged([this] { updateActions(); });
    m_browserWidget->setOnActivated([this] { applyAsDisplay(); });
    m_browserWidget->setOnContextMenuRequested([this](const QPoint&) { showContextMenu(); });
}

void CrsManagerDock::updateActions()
{
    if (!m_browserWidget || !m_favoriteAction) {
        for (QAction* action : { m_favoriteAction, m_editAction, m_promoteAction, m_deleteAction }) {
            if (action)
                action->setEnabled(false);
        }
        return;
    }
    const bool hasRow = m_browserWidget->hasSelection();
    const bool custom = m_browserWidget->currentIsCustom();
    const bool favorite =
        m_library && m_library->isFavorite(m_browserWidget->currentDefinition());
    m_favoriteAction->setEnabled(hasRow);
    m_favoriteAction->setText(favorite ? QStringLiteral("取消收藏") : QStringLiteral("收藏"));
    m_favoriteAction->setIcon(uiIcon(favorite ? QStringLiteral("action-favorite-on")
                                              : QStringLiteral("action-favorite")));
    m_editAction->setEnabled(hasRow && custom);
    m_promoteAction->setEnabled(hasRow && !custom);
    m_promoteAction->setToolTip(custom ? QStringLiteral("这条已经在自定义里了")
                                       : QStringLiteral("复制成一条可编辑的自定义记录"));
    m_deleteAction->setEnabled(hasRow && custom);
    m_deleteAction->setToolTip(hasRow && custom ? QString()
                                                : QStringLiteral("只有自定义条目能删"));
}

void CrsManagerDock::showContextMenu()
{
    updateActions();
    QMenu menu(m_widget);
    menu.addAction(m_favoriteAction);
    menu.addSeparator();
    menu.addAction(m_promoteAction);
    menu.addAction(m_editAction);
    menu.addAction(m_deleteAction);
    menu.exec(QCursor::pos());
}

void CrsManagerDock::startNewEntry()
{
    if (!m_library || !m_browser) {
        setStatus(QStringLiteral("参照系服务还没就绪"), true);
        return;
    }
    CrsEditorDialog dialog(m_browser, m_widget->window());
    if (dialog.exec() != QDialog::Accepted)
        return;
    View3D::CrsLibraryEntry added;
    QString error;
    if (!m_library->add(dialog.entryName(), dialog.definition(), dialog.note(), &added, &error)) {
        setStatus(error.isEmpty() ? QStringLiteral("新建参照系失败") : error, true);
        return;
    }
    m_browserWidget->refresh();
    m_browserWidget->selectCustomBranch();
    if (persist())
        setStatus(QStringLiteral("已新建「%1」，已写入 %2").arg(added.name, m_library->filePath()));
}

void CrsManagerDock::editCurrent()
{
    if (!m_library || !m_browser || !m_browserWidget)
        return;
    if (!m_browserWidget->currentIsCustom()) {
        setStatus(QStringLiteral("内置参照系改不了：先右键「存进自定义…」再编辑"), true);
        return;
    }
    const QString id = m_browserWidget->currentLibraryId();
    View3D::CrsLibraryEntry entry;
    for (const View3D::CrsLibraryEntry& candidate : m_library->entries()) {
        if (candidate.id == id) {
            entry = candidate;
            break;
        }
    }
    if (entry.id.isEmpty()) {
        setStatus(QStringLiteral("找不到这条自定义参照系"), true);
        return;
    }
    CrsEditorDialog dialog(m_browser, m_widget->window());
    dialog.setEntry(entry.name, entry.definition, entry.note);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    if (!m_library->update(id, dialog.entryName(), dialog.definition(), dialog.note(), &error)) {
        setStatus(error.isEmpty() ? QStringLiteral("保存失败") : error, true);
        return;
    }
    m_browserWidget->refresh();
    if (persist())
        setStatus(QStringLiteral("已保存「%1」，已写入 %2")
                      .arg(dialog.entryName().isEmpty() ? dialog.definition() : dialog.entryName(),
                           m_library->filePath()));
}

void CrsManagerDock::promoteToCustom()
{
    if (!m_library || !m_browser || !m_browserWidget)
        return;
    if (!m_browserWidget->hasSelection()) {
        setStatus(QStringLiteral("先选中一条参照系"), true);
        return;
    }
    if (m_browserWidget->currentIsCustom()) {
        setStatus(QStringLiteral("这条已经在自定义里了，直接右键「编辑…」"));
        return;
    }
    const QString definition = m_browserWidget->currentDefinition();
    CrsEditorDialog dialog(m_browser, m_widget->window());
    View3D::ICrsBrowser::Info info;
    if (m_browser->info(definition, &info)) {
        dialog.setEntry(info.name.isEmpty() ? definition : info.name, definition,
                        QStringLiteral("来自 %1 内置库")
                            .arg(info.code.isEmpty() ? QStringLiteral("EPSG") : info.code));
    } else {
        dialog.setEntry(QString(), definition, QString());
    }
    if (dialog.exec() != QDialog::Accepted)
        return;
    View3D::CrsLibraryEntry added;
    QString error;
    if (!m_library->add(dialog.entryName(), dialog.definition(), dialog.note(), &added, &error)) {
        setStatus(error.isEmpty() ? QStringLiteral("存进自定义失败") : error, true);
        return;
    }
    m_browserWidget->refresh();
    m_browserWidget->selectCustomBranch();
    if (persist())
        setStatus(QStringLiteral("已把「%1」存进自定义，已写入 %2")
                      .arg(added.name, m_library->filePath()));
}

void CrsManagerDock::removeCurrent()
{
    if (!m_library || !m_browserWidget)
        return;
    if (!m_browserWidget->currentIsCustom()) {
        setStatus(QStringLiteral("只有「自定义」里的条目能删，内置与常用项不能删"), true);
        return;
    }
    const QString id = m_browserWidget->currentLibraryId();
    QString name;
    for (const View3D::CrsLibraryEntry& candidate : m_library->entries()) {
        if (candidate.id == id) {
            name = candidate.name;
            break;
        }
    }
    if (id.isEmpty()) {
        setStatus(QStringLiteral("找不到这条自定义参照系"), true);
        return;
    }
    // 父窗口取主窗口：面板自身可能是隐藏的，确认框挂在它下面会显示不出来。
    if (QMessageBox::question(m_widget->window(), QStringLiteral("删除参照系"),
                              QStringLiteral("确定删除「%1」？库文件会立刻更新。").arg(name)) !=
        QMessageBox::Yes)
        return;
    m_library->remove(id);
    m_browserWidget->refresh();
    if (persist())
        setStatus(QStringLiteral("已删除「%1」，已写入 %2").arg(name, m_library->filePath()));
}

void CrsManagerDock::toggleFavorite()
{
    if (!m_library || !m_browserWidget)
        return;
    const QString definition = m_browserWidget->currentDefinition();
    if (definition.isEmpty()) {
        setStatus(QStringLiteral("先选中一条参照系"), true);
        return;
    }
    const bool now = !m_library->isFavorite(definition);
    m_library->setFavorite(definition, now);
    m_browserWidget->refresh();
    if (persist()) {
        setStatus(now ? QStringLiteral("已收藏「%1」，已写入 %2")
                            .arg(definition, m_library->filePath())
                      : QStringLiteral("已取消收藏「%1」，已写入 %2")
                            .arg(definition, m_library->filePath()));
    }
}

void CrsManagerDock::applyAsDisplay()
{
    if (!m_library || !m_reference || !m_browserWidget)
        return;
    const QString definition = m_browserWidget->currentDefinition();
    if (definition.isEmpty()) {
        setStatus(QStringLiteral("先选中一条参照系"), true);
        return;
    }
    if (!m_reference->setDisplayCrs(definition)) {
        setStatus(QStringLiteral("启用失败：%1").arg(m_reference->lastError()), true);
        return;
    }
    QString name = definition;
    View3D::ICrsBrowser::Info info;
    if (m_browser && m_browser->info(definition, &info) && !info.name.isEmpty())
        name = info.name;
    QString message = QStringLiteral("已启用「%1」作为显示参照系（动态投影开关随之打开）").arg(name);
    double lon = 0.0;
    double lat = 0.0;
    double height = 0.0;
    View3D::IGeoReference::PlaneFrame frame {};
    if (m_reference->origin(&lon, &lat, &height) &&
        m_reference->planeFrameAt(m_reference->displayCrs(), lon, lat, height, &frame)) {
        message += QStringLiteral("\n网格北相对真北收敛 %1°，投影比例因子 %2")
                       .arg(frame.convergenceDeg, 0, 'f', 4)
                       .arg(frame.scaleFactor, 0, 'f', 6);
    }
    setStatus(message);
}

void CrsManagerDock::importLibrary()
{
    if (!m_library)
        return;
    const QStringList paths = QFileDialog::getOpenFileNames(
        m_widget->window(), QStringLiteral("导入参照系定义"), QDir::homePath(),
        QStringLiteral("参照系定义 (*.prj *.wkt *.json);;所有文件 (*)"));
    if (paths.isEmpty())
        return;
    QStringList failed;
    QString error;
    const int added = m_library->importFiles(paths, &failed, &error);
    m_browserWidget->refresh();
    m_browserWidget->selectCustomBranch();
    const bool saved = persist();
    QString message = QStringLiteral("导入完成：新增 %1 条").arg(added);
    if (saved)
        message += QStringLiteral("，已写入 %1").arg(m_library->filePath());
    if (!failed.isEmpty())
        message += QStringLiteral("；失败 %1 个（%2）")
                       .arg(failed.size())
                       .arg(failed.join(QStringLiteral("、")));
    if (saved)
        setStatus(message, added == 0);
}

void CrsManagerDock::exportLibrary()
{
    if (!m_library)
        return;
    const QString path = QFileDialog::getSaveFileName(
        m_widget->window(), QStringLiteral("导出参照系"),
        QDir::homePath() + QStringLiteral("/crs_export.json"),
        QStringLiteral("JSON 库 (*.json);;WKT 定义 (*.prj *.wkt)"));
    if (path.isEmpty())
        return;
    QStringList ids;
    if (m_browserWidget && m_browserWidget->currentIsCustom())
        ids.append(m_browserWidget->currentLibraryId());
    else {
        for (const View3D::CrsLibraryEntry& entry : m_library->entries())
            ids.append(entry.id);
    }
    QString error;
    if (!m_library->exportFile(path, ids, &error)) {
        setStatus(error.isEmpty() ? QStringLiteral("导出失败") : error, true);
        return;
    }
    setStatus(QStringLiteral("已导出 %1 条到 %2").arg(ids.size()).arg(path));
}

bool CrsManagerDock::persist()
{
    if (!m_library)
        return false;
    QString error;
    if (!m_library->save(&error)) {
        setStatus(error.isEmpty() ? QStringLiteral("写库失败") : error, true);
        return false;
    }
    return true;
}

void CrsManagerDock::setStatus(const QString& text, bool failed)
{
    if (!m_status)
        return;
    m_status->setText(failed ? QStringLiteral("！%1").arg(text) : text);
}

} // namespace View3DCrs

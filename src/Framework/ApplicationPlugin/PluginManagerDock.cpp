#include "PluginManagerDock.h"
#include "ThemeController.h"

#include <IApplication.h>
#include <IDockWindow.h>
#include <IPluginContainer.h>
#include <IPluginRegistryService.h>
#include <PluginFactoryRegistry.h>
#include <PluginServiceIds.h>

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace View3DApplication {
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

class PluginManagerDock final : public RF::PluginEngine::IDockWindow {
  public:
    explicit PluginManagerDock(RF::PluginEngine::PluginCreationContext context)
        : m_context(std::move(context))
    {
        buildUi();
    }

    QString getCaption() const override
    {
        return m_context.attributes.value(QStringLiteral("text"),
                                          QStringLiteral("插件管理"));
    }
    RF::PluginEngine::DockingStyle getDefaultDock() const override
    {
        return RF::PluginEngine::DockingStyle::Right;
    }
    bool getInitCreate() const override { return true; }
    QString getName() const override
    {
        return m_context.attributes.value(QStringLiteral("name"),
                                          QStringLiteral("dw.plugins"));
    }
    void onActive(bool) override {}
    void onCreate(RF::PluginEngine::IApplication* app) override
    {
        m_application = app;
        refreshPackages();
    }
    void onDestroy() override { m_application = nullptr; }
    QWidget* getWidget() const override { return m_widget; }

  private:
    RF::PluginEngine::IPluginRegistryService* registry() const
    {
        auto* container = m_application ? m_application->getPluginContainer() : nullptr;
        return container
            ? container->objectAs<RF::PluginEngine::IPluginRegistryService>(
                  RF::PluginEngine::PluginRegistryServiceId)
            : nullptr;
    }

    QList<RF::PluginEngine::PluginPackageStatus> packages() const
    {
        auto* service = registry();
        return service ? service->packages()
                       : QList<RF::PluginEngine::PluginPackageStatus>{};
    }

    void buildUi()
    {
        m_widget = new QWidget();
        m_widget->setObjectName(getName());
        auto* layout = new QVBoxLayout(m_widget);
        layout->setContentsMargins(4, 4, 4, 4);

        m_summary = new QLabel(m_widget);
        m_summary->setObjectName(QStringLiteral("dw.plugins.summary"));
        layout->addWidget(m_summary);

        m_table = new QTableWidget(0, 6, m_widget);
        m_table->setObjectName(QStringLiteral("dw.plugins.table"));
        m_table->setHorizontalHeaderLabels(
            {QStringLiteral("插件 ID"), QStringLiteral("路径"), QStringLiteral("必需"),
             QStringLiteral("启用"), QStringLiteral("状态"), QStringLiteral("服务")});
        m_table->horizontalHeader()->setStretchLastSection(true);
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_table->verticalHeader()->setVisible(false);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        layout->addWidget(m_table, 1);

        auto* actions = new QHBoxLayout();
        auto* details = new QPushButton(QStringLiteral("详情"), m_widget);
        details->setObjectName(QStringLiteral("dw.plugins.details"));
        details->setIcon(uiIcon(QStringLiteral("action-details")));
        auto* refresh = new QPushButton(QStringLiteral("刷新"), m_widget);
        refresh->setObjectName(QStringLiteral("dw.plugins.refresh"));
        refresh->setIcon(uiIcon(QStringLiteral("action-refresh")));
        actions->addWidget(details);
        actions->addWidget(refresh);
        actions->addStretch(1);
        layout->addLayout(actions);

        QObject::connect(details, &QPushButton::clicked, m_widget, [this] { showDetails(); });
        QObject::connect(refresh, &QPushButton::clicked, m_widget,
                         [this] { refreshPackages(); });
        refreshPackages();
    }

    void refreshPackages()
    {
        if (!m_table || !m_summary)
            return;
        const auto service = registry();
        const auto statuses = packages();
        m_table->setRowCount(statuses.size());
        int loaded = 0;
        for (int row = 0; row < statuses.size(); ++row) {
            const RF::PluginEngine::PluginPackageStatus& status = statuses.at(row);
            if (status.loaded)
                ++loaded;
            QString state = QStringLiteral("未加载");
            if (status.loaded)
                state = QStringLiteral("已加载");
            else if (!status.error.isEmpty())
                state = QStringLiteral("失败：%1").arg(status.error);
            m_table->setItem(row, 0, new QTableWidgetItem(status.id));
            m_table->setItem(row, 1, new QTableWidgetItem(status.path));
            m_table->setItem(row, 2,
                             new QTableWidgetItem(status.required ? QStringLiteral("是")
                                                                  : QStringLiteral("否")));
            m_table->setItem(row, 3,
                             new QTableWidgetItem(status.enabled ? QStringLiteral("是")
                                                                 : QStringLiteral("否")));
            m_table->setItem(row, 4, new QTableWidgetItem(state));
            m_table->setItem(row, 5,
                             new QTableWidgetItem(status.provides.join(QStringLiteral(" "))));
        }
        m_summary->setText(service
                               ? QStringLiteral("已加载 %1 / 共 %2 个插件")
                                     .arg(loaded)
                                     .arg(statuses.size())
                               : QStringLiteral("插件注册表不可用"));
    }

    void showDetails()
    {
        const int row = m_table ? m_table->currentRow() : -1;
        const auto statuses = packages();
        if (row < 0 || row >= statuses.size())
            return;
        const RF::PluginEngine::PluginPackageStatus& status = statuses.at(row);
        QWidget* parent = m_application ? m_application->getMainWindow() : nullptr;
        QDialog dialog(parent);
        dialog.setWindowTitle(QStringLiteral("插件详情 - %1").arg(status.id));
        dialog.resize(620, 420);
        auto* layout = new QVBoxLayout(&dialog);
        auto* details = new QPlainTextEdit(&dialog);
        details->setReadOnly(true);
        QStringList lines;
        lines << QStringLiteral("插件 ID：%1").arg(status.id)
              << QStringLiteral("路径：%1").arg(status.path)
              << QStringLiteral("必需：%1")
                     .arg(status.required ? QStringLiteral("是") : QStringLiteral("否"))
              << QStringLiteral("启用：%1")
                     .arg(status.enabled ? QStringLiteral("是") : QStringLiteral("否"))
              << QStringLiteral("加载状态：%1")
                     .arg(status.loaded
                              ? QStringLiteral("已加载")
                              : (status.error.isEmpty() ? QStringLiteral("未加载")
                                                        : QStringLiteral("失败")));
        if (!status.error.isEmpty())
            lines << QStringLiteral("错误：%1").arg(status.error);
        lines << QStringLiteral("依赖：%1")
                     .arg(status.requiresPlugins.isEmpty()
                              ? QStringLiteral("（无）")
                              : status.requiresPlugins.join(QStringLiteral(", ")))
              << QStringLiteral("可选依赖：%1")
                     .arg(status.optionalPlugins.isEmpty()
                              ? QStringLiteral("（无）")
                              : status.optionalPlugins.join(QStringLiteral(", ")))
              << QStringLiteral("提供服务：%1")
                     .arg(status.provides.isEmpty()
                              ? QStringLiteral("（无）")
                              : status.provides.join(QStringLiteral(", ")));
        details->setPlainText(lines.join(QLatin1Char('\n')));
        layout->addWidget(details);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.exec();
    }

    RF::PluginEngine::PluginCreationContext m_context;
    RF::PluginEngine::IApplication* m_application = nullptr;
    QWidget* m_widget = nullptr;
    QLabel* m_summary = nullptr;
    QTableWidget* m_table = nullptr;
};

} // namespace

RF::PluginEngine::IPlugin* createPluginManagerDock(
    const RF::PluginEngine::PluginCreationContext& context)
{
    return new PluginManagerDock(context);
}

} // namespace View3DApplication

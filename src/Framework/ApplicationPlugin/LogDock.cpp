#include "LogDock.h"

#include <IAppLogManager.h>
#include <IApplication.h>
#include <PluginFactoryRegistry.h>

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageLogContext>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QThread>
#include <QTime>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace View3DApplication {
namespace {

using RF::PluginEngine::AppLogLevel;

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

// 面板只留最近这么多条：交付版可能开几小时，无限增长会把内存吃掉。
constexpr int kMaxRecords = 5000;

QString levelText(AppLogLevel level)
{
    switch (level) {
    case AppLogLevel::Debug:
        return QStringLiteral("调试");
    case AppLogLevel::Warn:
        return QStringLiteral("警告");
    case AppLogLevel::Error:
        return QStringLiteral("错误");
    case AppLogLevel::Fatal:
        return QStringLiteral("致命");
    case AppLogLevel::Info:
    default:
        return QStringLiteral("信息");
    }
}

AppLogLevel levelOfQtMessage(const QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
    case QtInfoMsg:
        return AppLogLevel::Info;
    case QtWarningMsg:
        return AppLogLevel::Warn;
    case QtCriticalMsg:
    case QtFatalMsg:
        return AppLogLevel::Error;
    }
    return AppLogLevel::Info;
}

struct Record {
    QTime time;
    QString logger;
    AppLogLevel level = AppLogLevel::Info;
    QString message;
    QString details;

    QString line() const
    {
        QString text = QStringLiteral("[%1] [%2] %3: %4")
                           .arg(time.toString(QStringLiteral("HH:mm:ss")), levelText(level),
                                logger, message);
        if (!details.isEmpty())
            text += QStringLiteral("\n        %1").arg(details);
        return text;
    }
};

} // namespace

class LogView final : public QWidget {
  public:
    explicit LogView(QWidget* parent = nullptr) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        auto* tools = new QHBoxLayout();
        tools->addWidget(new QLabel(QStringLiteral("级别"), this));
        m_level = new QComboBox(this);
        m_level->setObjectName(QStringLiteral("logLevel"));
        m_level->addItems({QStringLiteral("全部"), QStringLiteral("信息"),
                           QStringLiteral("警告"), QStringLiteral("错误")});
        tools->addWidget(m_level);
        m_filter = new QLineEdit(this);
        m_filter->setObjectName(QStringLiteral("logFilter"));
        m_filter->setPlaceholderText(QStringLiteral("过滤日志内容…"));
        m_filter->setClearButtonEnabled(true);
        tools->addWidget(m_filter, 1);
        m_clear = new QPushButton(QStringLiteral("清空"), this);
        m_clear->setObjectName(QStringLiteral("logClear"));
        m_clear->setIcon(uiIcon(QStringLiteral("action-clear")));
        tools->addWidget(m_clear);
        layout->addLayout(tools);

        m_text = new QPlainTextEdit(this);
        m_text->setObjectName(QStringLiteral("logText"));
        m_text->setReadOnly(true);
        m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_text->setMaximumBlockCount(kMaxRecords);
        layout->addWidget(m_text, 1);

        m_level->setCurrentIndex(0);
        QObject::connect(m_level, &QComboBox::currentTextChanged, this, [this] { reload(); });
        QObject::connect(m_filter, &QLineEdit::textChanged, this, [this] { reload(); });
        QObject::connect(m_clear, &QPushButton::clicked, this,
                         [this] {
                             m_records.clear();
                             m_text->clear();
                         });
    }

    // 插件可能在工作线程写日志，这里统一投递到界面线程再落笔。
    void submit(Record record)
    {
        if (QThread::currentThread() == this->thread()) {
            appendNow(std::move(record));
            return;
        }
        QMetaObject::invokeMethod(this, [this, record] { appendNow(record); },
                                  Qt::QueuedConnection);
    }

    int recordCount() const { return static_cast<int>(m_records.size()); }
    int visibleLineCount() const { return m_text->blockCount(); }
    bool bufferContains(const QString& needle) const
    {
        for (const Record& record : m_records) {
            if (record.message.contains(needle, Qt::CaseInsensitive) ||
                record.logger.contains(needle, Qt::CaseInsensitive) ||
                record.details.contains(needle, Qt::CaseInsensitive))
                return true;
        }
        return false;
    }

  private:
    bool matches(const Record& record) const
    {
        const int levelIndex = m_level->currentIndex();
        if (levelIndex == 1 && record.level != AppLogLevel::Info &&
            record.level != AppLogLevel::Debug)
            return false;
        if (levelIndex == 2 && record.level != AppLogLevel::Warn)
            return false;
        if (levelIndex == 3 && record.level != AppLogLevel::Error &&
            record.level != AppLogLevel::Fatal)
            return false;
        const QString text = m_filter->text().trimmed();
        if (text.isEmpty())
            return true;
        return record.message.contains(text, Qt::CaseInsensitive) ||
            record.logger.contains(text, Qt::CaseInsensitive) ||
            record.details.contains(text, Qt::CaseInsensitive);
    }

    void appendNow(Record record)
    {
        record.time = QTime::currentTime();
        if (m_records.size() >= kMaxRecords)
            m_records.removeFirst();
        m_records.append(record);
        if (!matches(record))
            return;
        const bool atBottom =
            m_text->verticalScrollBar()->value() >= m_text->verticalScrollBar()->maximum();
        m_text->appendPlainText(record.line());
        if (atBottom)
            scrollToEnd();
    }

    void reload()
    {
        QStringList lines;
        for (const Record& record : m_records) {
            if (matches(record))
                lines.append(record.line());
        }
        const bool atBottom =
            m_text->verticalScrollBar()->value() >= m_text->verticalScrollBar()->maximum();
        m_text->setPlainText(lines.join(QLatin1Char('\n')));
        if (atBottom)
            scrollToEnd();
    }

    void scrollToEnd()
    {
        auto* bar = m_text->verticalScrollBar();
        bar->setValue(bar->maximum());
    }

    QList<Record> m_records;
    QPlainTextEdit* m_text = nullptr;
    QLineEdit* m_filter = nullptr;
    QComboBox* m_level = nullptr;
    QPushButton* m_clear = nullptr;
};


LogDock::LogDock(const RF::PluginEngine::PluginCreationContext& context)
    : m_name(context.attributes.value(QStringLiteral("name"))),
      m_caption(context.attributes.value(QStringLiteral("text"))),
      m_view(std::make_unique<LogView>())
{
    if (m_caption.isEmpty())
        m_caption = QStringLiteral("日志");
    m_view->setObjectName(QStringLiteral("view.log"));
}

LogDock::~LogDock()
{
    onDestroy();
}
void LogDock::onCreate(RF::PluginEngine::IApplication* application)
{
    m_application = application;
    attach();
}

void LogDock::onDestroy()
{
    detach();
    m_application = nullptr;
}

QWidget* LogDock::getWidget() const
{
    return m_view.get();
}

int LogDock::recordCount() const
{
    return m_view ? m_view->recordCount() : 0;
}

int LogDock::visibleLineCount() const
{
    return m_view ? m_view->visibleLineCount() : 0;
}

bool LogDock::bufferContains(const QString& needle) const
{
    return m_view && m_view->bufferContains(needle);
}

void LogDock::attach()
{
    auto* manager = m_application ? m_application->getAppLogManager() : nullptr;
    if (!manager || m_subscribeToken != -1)
        return;
    auto* view = m_view.get();
    m_subscribeToken = manager->subscribeLog([view](const QString& logger, AppLogLevel level,
                                                     const QString& message,
                                                     const QString& details) {
        if (!view)
            return;
        Record record;
        record.logger = logger;
        record.level = level;
        record.message = message;
        record.details = details;
        view->submit(std::move(record));
    });
}

void LogDock::detach()
{
    auto* manager = m_application ? m_application->getAppLogManager() : nullptr;
    if (manager && m_subscribeToken != -1)
        manager->unsubscribeLog(m_subscribeToken);
    m_subscribeToken = -1;
}

RF::PluginEngine::IPlugin* createLogDock(const RF::PluginEngine::PluginCreationContext& context)
{
    return new LogDock(context);
}

QtMessageHandler g_previousMessageHandler = nullptr;
RF::PluginEngine::IAppLogManager* g_logManager = nullptr;

// Qt 自己的告警（工厂不可用、发布服务缺类型、插件加载失败…）本来只出现在调试输出里，
// 交付版用户看不到。这里接进同一条日志管道，并且继续转交上一个 handler，
// 保持 stderr/调试器输出不变——冒烟检查正是靠那些标记判定。
void routeQtMessageToLog(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (g_logManager) {
        if (auto* logger = g_logManager->getLogger(QStringLiteral("qt"))) {
            const QString where = context.file
                ? QStringLiteral("%1:%2").arg(QString::fromUtf8(context.file)).arg(context.line)
                : QString();
            switch (levelOfQtMessage(type)) {
            case AppLogLevel::Warn:
                logger->warn(message, where);
                break;
            case AppLogLevel::Error:
                logger->error(message, where);
                break;
            default:
                logger->info(message, where);
                break;
            }
        }
    }
    if (g_previousMessageHandler)
        g_previousMessageHandler(type, context, message);
}

void installQtMessageLogBridge(RF::PluginEngine::IAppLogManager* manager)
{
    g_logManager = manager;
    g_previousMessageHandler = qInstallMessageHandler(routeQtMessageToLog);
}

void uninstallQtMessageLogBridge()
{
    if (!g_previousMessageHandler)
        return;
    qInstallMessageHandler(g_previousMessageHandler);
    g_previousMessageHandler = nullptr;
    g_logManager = nullptr;
}

} // namespace View3DApplication

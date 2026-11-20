#include "CrashDiagnostics.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QStringList>
#include <QSysInfo>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

#include <cstdio>
#include <cstdlib>

namespace {

QString g_dataDirectory;
QString g_pluginSnapshot;

QString crashTimestamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

QString crashFileName() {
    return QStringLiteral("crash-%1.log").arg(crashTimestamp());
}

#ifdef _WIN32
// 文本记录只能给出异常码；下一次崩溃要能直接看调用栈，所以同时写一个
// 进程内 minidump（不抓全内存，避免几个 GB）。写入失败不影响文本记录。
QString writeMiniDump(EXCEPTION_POINTERS *exceptionPointers) {
    const QString path =
        QDir(g_dataDirectory).filePath(QStringLiteral("crash-%1.dmp").arg(crashTimestamp()));
    const HANDLE file = CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    MINIDUMP_EXCEPTION_INFORMATION information {};
    information.ThreadId = GetCurrentThreadId();
    information.ExceptionPointers = exceptionPointers;
    information.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithThreadInfo);
    const BOOL written = MiniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), file, type,
        exceptionPointers ? &information : nullptr, nullptr, nullptr);
    CloseHandle(file);
    return written ? path : QString();
}
#endif

void writeCrashFile(const QStringList &lines) {
    if (g_dataDirectory.isEmpty())
        return;
    QDir().mkpath(g_dataDirectory);
    const QString path =
        QDir(g_dataDirectory).filePath(crashFileName());
    const QByteArray content =
        (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
#ifdef _WIN32
    FILE *file = nullptr;
    if (fopen_s(&file, path.toLocal8Bit().constData(), "ab") == 0 && file) {
        fwrite(content.constData(), 1, static_cast<size_t>(content.size()), file);
        fclose(file);
    }
#else
    FILE *file = std::fopen(path.toLocal8Bit().constData(), "ab");
    if (file) {
        std::fwrite(content.constData(), 1, static_cast<size_t>(content.size()), file);
        std::fclose(file);
    }
#endif
}

QStringList baseReportLines(const QString &reason) {
    QStringList lines;
    lines << QStringLiteral("三维数据预览系统 - 崩溃记录")
          << QStringLiteral("原因：%1").arg(reason)
          << QStringLiteral("时间：%1")
                 .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
          << QStringLiteral("应用：%1 %2")
                 .arg(QCoreApplication::applicationName(),
                      QCoreApplication::applicationVersion())
          << QStringLiteral("Qt：%1").arg(QString::fromLatin1(qVersion()))
          << QStringLiteral("系统：%1 (%2)")
                 .arg(QSysInfo::prettyProductName(),
                      QSysInfo::currentCpuArchitecture())
          << g_pluginSnapshot;
    return lines;
}

#ifdef _WIN32
LONG WINAPI unhandledFilter(_EXCEPTION_POINTERS *exceptionPointers) {
    QStringList lines = baseReportLines(QStringLiteral("未处理异常"));
    if (exceptionPointers && exceptionPointers->ExceptionRecord) {
        const EXCEPTION_RECORD *record = exceptionPointers->ExceptionRecord;
        lines << QStringLiteral("异常码：0x%1")
                     .arg(static_cast<quint64>(record->ExceptionCode), 8, 16,
                          QLatin1Char('0'));
        lines << QStringLiteral("异常地址：0x%1")
                     .arg(reinterpret_cast<quintptr>(record->ExceptionAddress), 0,
                          16);
    }
    const QString dumpPath = writeMiniDump(exceptionPointers);
    if (!dumpPath.isEmpty())
        lines << QStringLiteral("转储：%1").arg(dumpPath);
    writeCrashFile(lines);
    ExitProcess(1);
}
#endif

} // namespace

namespace View3DApplication {

void installCrashDiagnostics(const QString &applicationDataDirectory,
                             const QString &pluginSnapshot) {
    g_dataDirectory = applicationDataDirectory;
    g_pluginSnapshot =
        pluginSnapshot.isEmpty() ? QStringLiteral("插件快照：不可用")
                                 : QStringLiteral("插件快照：%1").arg(pluginSnapshot);
#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandledFilter);
#endif
    std::set_terminate([] {
        writeCrashFile(baseReportLines(QStringLiteral("std::terminate 被调用")));
        std::abort();
    });
}

} // namespace View3DApplication

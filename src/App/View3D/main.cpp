#include "LogTee.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QXmlStreamReader>

#include <iostream>

namespace {
struct MainApplicationPackage {
    QString path;
    QString entry = QStringLiteral("runConfiguredApplication");
};

MainApplicationPackage readMainApplicationPackage(const QString& configPath, QString* error)
{
    QFile input(configPath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error) *error = input.errorString();
        return {};
    }
    QXmlStreamReader xml(&input);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QStringLiteral("mainapp")) continue;
        const auto attributes = xml.attributes();
        MainApplicationPackage result;
        result.path = attributes.value(QStringLiteral("pluginPath")).toString();
        const QString configuredEntry = attributes.value(QStringLiteral("entry")).toString();
        if (!configuredEntry.isEmpty()) result.entry = configuredEntry;
        return result;
    }
    if (error) *error = xml.hasError() ? xml.errorString()
                                      : QStringLiteral("mainapp pluginPath is not configured");
    return {};
}

QString configPathFromArguments(int argc, char* argv[])
{
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument.startsWith(QStringLiteral("--ui-config=")))
            return QFileInfo(argument.mid(12)).absoluteFilePath();
        if (index + 1 < argc && argument == QStringLiteral("--ui-config"))
            return QFileInfo(QString::fromLocal8Bit(argv[index + 1])).absoluteFilePath();
    }
    const QString executable = QFileInfo(QString::fromLocal8Bit(argv[0])).absoluteFilePath();
    return QFileInfo(executable).absoluteDir().filePath(QStringLiteral("ui_config.xml"));
}
}

int main(int argc, char* argv[])
{
    // WIN32 子系统没有控制台：插件打印的 VIEW3D_* 诊断必须落盘，否则换机器排查时
    // 现场只剩 Ogre 自己的日志。放在读配置之前，宿主自己的失败也能留下痕迹。
    const QString executablePath = QFileInfo(QString::fromLocal8Bit(argv[0])).absoluteFilePath();
    installLogTee(QFileInfo(executablePath).absoluteDir().filePath(QStringLiteral("View3D.log")));
    const QString configPath = configPathFromArguments(argc, argv);
    QString error;
    const MainApplicationPackage package = readMainApplicationPackage(configPath, &error);
    if (package.path.isEmpty()) {
        std::cerr << "APPLICATION_HOST_CONFIG_FAILED: " << error.toStdString() << '\n';
        return 201;
    }
    const QDir configDirectory = QFileInfo(configPath).absoluteDir();
    const QString libraryPath = QFileInfo(package.path).isAbsolute()
        ? package.path : configDirectory.filePath(package.path);
    QLibrary library(QDir::cleanPath(libraryPath));
    if (!library.load()) {
        std::cerr << "APPLICATION_HOST_LOAD_FAILED: "
                  << library.errorString().toStdString() << '\n';
        return 202;
    }
    using EntryPoint = int (*)(int, char**);
    const auto entry = reinterpret_cast<EntryPoint>(library.resolve(package.entry.toLatin1()));
    if (!entry) {
        std::cerr << "APPLICATION_HOST_ENTRY_FAILED: "
                  << library.errorString().toStdString() << '\n';
        return 203;
    }
    return entry(argc, argv);
}

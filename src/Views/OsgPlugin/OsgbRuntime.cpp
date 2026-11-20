#include "OsgbRuntime.h"

#include <QCoreApplication>
#include <QDir>

namespace View3DOsb {

QStringList osgPluginRoots()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    QStringList roots;
    roots << QDir::cleanPath(appDir.filePath(QStringLiteral("plugins")));
    roots << QDir::cleanPath(appDir.absolutePath());
    return roots;
}

QStringList osgPluginDirs()
{
    QStringList dirs;
    for (const QString& root : osgPluginRoots()) {
        const QDir dir(root);
        const QStringList found = dir.entryList({ QStringLiteral("osgPlugins-*") },
                                                QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& entry : found)
            dirs << QDir::cleanPath(dir.absoluteFilePath(entry));
    }
    return dirs;
}

QString osgPluginSearchHint()
{
    QStringList patterns;
    for (const QString& root : osgPluginRoots())
        patterns << QDir::toNativeSeparators(root + QStringLiteral("\\osgPlugins-*"));
    return patterns.join(QStringLiteral(" 或 "));
}

} // namespace View3DOsb

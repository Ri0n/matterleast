#include <QtTest>

#include <QDir>

#include "plugins/PluginSearchPathPolicy.h"

using namespace Mattermost;

class PluginSearchPathPolicyTest : public QObject
{
    Q_OBJECT

    static QString location(const QString& relative)
    {
        return QDir::cleanPath(
            QDir(QDir::tempPath()).filePath(
                QStringLiteral("matterleast-plugin-path-policy/") + relative));
    }

private slots:
    void installedLinuxUsesLibdirNotBinPlugins()
    {
        const QString appDir = location(QStringLiteral("installed/bin"));
        const QString buildDir = location(QStringLiteral("build"));
        const QString userData = location(QStringLiteral("home/data"));
        const QStringList paths = resolveNativePluginSearchPaths(
            appDir, buildDir,
            QStringLiteral("../lib/x86_64-linux-gnu/matterleast/plugins"),
            userData);

        QCOMPARE(paths.size(), 2);
        QCOMPARE(paths.at(0),
                 location(QStringLiteral("installed/lib/x86_64-linux-gnu/matterleast/plugins")));
        QCOMPARE(paths.at(1), location(QStringLiteral("home/data/plugins")));
        QVERIFY(!paths.contains(QDir(appDir).filePath(QStringLiteral("plugins"))));
    }

    void linuxCustomPrefixAndLib64()
    {
        const QString appDir = location(QStringLiteral("custom-prefix/bin"));
        const QStringList paths = resolveNativePluginSearchPaths(
            appDir, location(QStringLiteral("build")),
            QStringLiteral("../lib64/matterleast/plugins"),
            QString());

        QCOMPARE(paths, QStringList {
            location(QStringLiteral("custom-prefix/lib64/matterleast/plugins"))
        });
    }

    void uninstalledLinuxBuildKeepsAdjacentPlugins()
    {
        const QString buildDir = location(QStringLiteral("build"));
        const QString userData = location(QStringLiteral("home/data"));
        const QStringList paths = resolveNativePluginSearchPaths(
            buildDir, buildDir, QStringLiteral("../lib/matterleast/plugins"),
            userData);

        QCOMPARE(paths.size(), 3);
        QCOMPARE(paths.at(0), location(QStringLiteral("build/plugins")));
        QCOMPARE(paths.at(1), location(QStringLiteral("lib/matterleast/plugins")));
        QCOMPARE(paths.at(2), location(QStringLiteral("home/data/plugins")));
    }

    void multiConfigLinuxBuildKeepsAdjacentPlugins()
    {
        const QString buildDir = location(QStringLiteral("build/Debug"));
        const QStringList paths = resolveNativePluginSearchPaths(
            buildDir, buildDir,
            QStringLiteral("../../lib/matterleast/plugins"), QString());

        QCOMPARE(paths.first(), location(QStringLiteral("build/Debug/plugins")));
        QCOMPARE(paths.size(), 2);
    }

    void executableLocalBundleStillWorksWithoutSystemLayout()
    {
        const QString appDir = location(QStringLiteral("bundle/bin"));
        const QStringList paths = resolveNativePluginSearchPaths(
            appDir, QString(), QString(), location(QStringLiteral("home/data")));

        QCOMPARE(paths.size(), 2);
        QCOMPARE(paths.at(0), location(QStringLiteral("bundle/bin/plugins")));
        QCOMPARE(paths.at(1), location(QStringLiteral("home/data/plugins")));
    }
};

QTEST_GUILESS_MAIN(PluginSearchPathPolicyTest)
#include "PluginSearchPathPolicyTest.moc"

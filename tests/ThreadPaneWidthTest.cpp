#include <QtTest>
#include <QSplitter>
#include <QWidget>

#include "navigation/ThreadPaneLayout.h"

using namespace Mattermost;

class ThreadPaneWidthTest final : public QObject
{
    Q_OBJECT
private slots:
    void retainsLastDockedWidthWhenThreadIsHidden()
    {
        QSplitter splitter(Qt::Horizontal);
        QWidget mainPane;
        QWidget threadPane;
        mainPane.setMinimumWidth(150);
        threadPane.setMinimumWidth(200);
        splitter.addWidget(&mainPane);
        splitter.addWidget(&threadPane);
        splitter.resize(900, 500);
        splitter.show();
        QCoreApplication::processEvents();
        splitter.setSizes({550, 346});
        QCoreApplication::processEvents();

        const int preferred = expandedThreadPaneWidth(splitter);
        QVERIFY(preferred > 0);
        threadPane.hide();
        QCoreApplication::processEvents();
        QCOMPARE(expandedThreadPaneWidth(splitter), 0);
        QVERIFY(!restoreThreadPaneWidth(splitter, preferred));

        threadPane.show();
        QCoreApplication::processEvents();
        QVERIFY(restoreThreadPaneWidth(splitter, preferred));
        QVERIFY(qAbs(expandedThreadPaneWidth(splitter) - preferred) <= 1);
    }

    void invalidWidthIsNeverApplied()
    {
        QSplitter splitter(Qt::Horizontal);
        QWidget first;
        QWidget second;
        splitter.addWidget(&first);
        splitter.addWidget(&second);
        splitter.resize(800, 400);
        splitter.show();
        QCoreApplication::processEvents();

        QVERIFY(!restoreThreadPaneWidth(splitter, 0));
        QVERIFY(!restoreThreadPaneWidth(splitter, -1));
    }
};

QTEST_MAIN(ThreadPaneWidthTest)
#include "ThreadPaneWidthTest.moc"

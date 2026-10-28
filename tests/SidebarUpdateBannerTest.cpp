#include <QtTest>

#include "ui/SidebarUpdateBanner.h"

#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>

using Mattermost::SidebarUpdateBanner;

class SidebarUpdateBannerTest : public QObject
{
    Q_OBJECT

private slots:
    void hiddenUntilAnUpdateIsFound()
    {
        QWidget window;
        SidebarUpdateBanner banner(&window);
        window.show();
        QVERIFY(banner.isHidden());
        banner.showUpdate(SidebarUpdateBanner::Phase::Ready,
                          QStringLiteral("nightly-20261029-010000-abcdef123456"));
        QVERIFY(banner.isVisible());
        banner.clearUpdate();
        QVERIFY(banner.isHidden());
    }

    void downloadingRemainsClickableAndShowsProgress()
    {
        QWidget window;
        SidebarUpdateBanner banner(&window);
        window.show();
        QSignalSpy activated(&banner, &SidebarUpdateBanner::activated);
        banner.showUpdate(SidebarUpdateBanner::Phase::Downloading,
                          QStringLiteral("nightly-20261029-010000-abcdef123456"), 42);
        auto* button = banner.findChild<QPushButton*>(QStringLiteral("sidebarUpdateButton"));
        auto* progress = banner.findChild<QProgressBar*>(QStringLiteral("sidebarUpdateProgress"));
        QVERIFY(button);
        QVERIFY(progress);
        QVERIFY(button->isEnabled());
        QVERIFY(button->text().contains(QStringLiteral("42%")));
        QVERIFY(progress->isVisible());
        QCOMPARE(progress->value(), 42);

        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(activated.size(), 1);
        banner.showUpdate(SidebarUpdateBanner::Phase::Downloading,
                          QStringLiteral("nightly-20261029-010000-abcdef123456"), 68, true);
        QVERIFY(button->text().contains(QStringLiteral("Restart after download")));

        banner.showUpdate(SidebarUpdateBanner::Phase::Ready,
                          QStringLiteral("nightly-20261029-010000-abcdef123456"));
        QVERIFY(!progress->isVisible());
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(activated.size(), 2);
    }

    void failedUpdateRemainsRetryable()
    {
        QWidget window;
        SidebarUpdateBanner banner(&window);
        window.show();
        banner.showUpdate(SidebarUpdateBanner::Phase::Failed,
                          QStringLiteral("nightly-20261029-010000-abcdef123456"),
                          -1, false, QStringLiteral("Download failed"));
        const auto* button = banner.findChild<QPushButton*>(QStringLiteral("sidebarUpdateButton"));
        QVERIFY(button);
        QVERIFY(button->text().contains(QStringLiteral("Retry")));
        QVERIFY(button->toolTip().contains(QStringLiteral("Download failed")));
    }
};

QTEST_MAIN(SidebarUpdateBannerTest)
#include "SidebarUpdateBannerTest.moc"

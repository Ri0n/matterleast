#include <algorithm>

#include <QtTest>

#include <QApplication>
#include <QPalette>
#include <QTabWidget>
#include <QWidget>

#include "choose-emoji-dialog/EmojiDialogSupport.h"
#include "reactions/ReactionUsage.h"
#include "ui/EmojiFont.h"

class EmojiDialogSupportTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizesMattermostStyleQueries()
    {
        using Mattermost::EmojiDialogSupport::normalizeSearchTerm;

        QCOMPARE(normalizeSearchTerm(QStringLiteral(" :Rolling-On The-Floor: ")),
                 QStringLiteral("rolling_on_the_floor"));
        QCOMPARE(normalizeSearchTerm(QStringLiteral("pizza")),
                 QStringLiteral("pizza"));
    }

    void preservesCustomEmojiPunctuationForServerSearch()
    {
        using Mattermost::EmojiDialogSupport::customEmojiServerSearchTerm;

        QCOMPARE(customEmojiServerSearchTerm(QStringLiteral(" :Approved-By_QA: ")),
                 QStringLiteral("approved-by_qa"));
        QCOMPARE(customEmojiServerSearchTerm(QStringLiteral("approved")),
                 QStringLiteral("approved"));
    }

    void matchesCanonicalEmojiNames()
    {
        using Mattermost::EmojiDialogSupport::matchesSearch;

        QVERIFY(matchesSearch(QStringLiteral("rolling_on_the_floor_laughing"),
                              QStringLiteral("rolling floor")));
        QVERIFY(matchesSearch(QStringLiteral("face_vomiting"),
                              QStringLiteral(":vomit:")));
        QVERIFY(!matchesSearch(QStringLiteral("floor_rolling"),
                               QStringLiteral("rolling floor")));
        QVERIFY(!matchesSearch(QStringLiteral("pizza"),
                               QStringLiteral("cherry")));
    }

    void emojiTabPagesTrackApplicationPaletteChanges()
    {
        QTabWidget tabWidget;
        QWidget page;
        Mattermost::EmojiDialogSupport::configureTabWidget(tabWidget);
        tabWidget.addTab(&page, QStringLiteral("Emoji"));

        QVERIFY(tabWidget.styleSheet().isEmpty());
        QVERIFY(tabWidget.tabBar()->minimumHeight() >= 42);

        const QPalette original = QApplication::palette();

        QPalette first = original;
        first.setColor(QPalette::Window, QColor(245, 245, 245));
        first.setColor(QPalette::WindowText, QColor(20, 20, 20));
        QApplication::setPalette(first);
        QApplication::processEvents();

        QCOMPARE(tabWidget.palette().color(QPalette::Window),
                 first.color(QPalette::Window));
        QCOMPARE(page.palette().color(QPalette::Window),
                 first.color(QPalette::Window));

        QPalette second = original;
        second.setColor(QPalette::Window, QColor(25, 25, 25));
        second.setColor(QPalette::WindowText, QColor(235, 235, 235));
        QApplication::setPalette(second);
        QApplication::processEvents();

        QCOMPARE(tabWidget.palette().color(QPalette::Window),
                 second.color(QPalette::Window));
        QCOMPARE(page.palette().color(QPalette::Window),
                 second.color(QPalette::Window));

        QApplication::setPalette(original);
        QApplication::processEvents();
    }

    void choosesPlatformLegacyEmojiFonts()
    {
        using Mattermost::EmojiFont::Platform;
        using Mattermost::EmojiFont::chooseLegacyEmojiFontFamily;

        const QStringList installed = {
            QStringLiteral("Arial"),
            QStringLiteral("Noto Color Emoji"),
            QStringLiteral("Segoe UI Emoji"),
            QStringLiteral("Apple Color Emoji"),
        };

        QCOMPARE(chooseLegacyEmojiFontFamily(installed, Platform::Linux),
                 QStringLiteral("Noto Color Emoji"));
        QCOMPARE(chooseLegacyEmojiFontFamily(installed, Platform::Windows),
                 QStringLiteral("Segoe UI Emoji"));
        QCOMPARE(chooseLegacyEmojiFontFamily(installed, Platform::MacOS),
                 QStringLiteral("Apple Color Emoji"));
        QVERIFY(chooseLegacyEmojiFontFamily({QStringLiteral("Arial")},
                                            Platform::Linux).isEmpty());
    }

    void preservesFoundryQualifiedFamilyName()
    {
        using Mattermost::EmojiFont::Platform;
        using Mattermost::EmojiFont::chooseLegacyEmojiFontFamily;

        const QStringList installed = {
            QStringLiteral("Noto Color Emoji [Google]"),
        };
        QCOMPARE(chooseLegacyEmojiFontFamily(installed, Platform::Linux),
                 QStringLiteral("Noto Color Emoji [Google]"));
    }

    void reheatsSelectedReactionToTheTop()
    {
        Mattermost::ReactionUsageModel model;
        model.recordUse(QStringLiteral("fire"));
        model.recordUse(QStringLiteral("eyes"));

        auto ranking = model.ranking();
        QCOMPARE(static_cast<int>(ranking.size()), 2);
        QCOMPARE(ranking.at(0).name, QStringLiteral("eyes"));
        QCOMPARE(ranking.at(0).count, quint64(1));
        QVERIFY(qAbs(ranking.at(0).heat - 1.0) < 1e-12);
        QCOMPARE(ranking.at(1).name, QStringLiteral("fire"));
        QVERIFY(qAbs(ranking.at(1).heat
                     - Mattermost::ReactionUsageModel::coolingFactor(1)) < 1e-12);

        model.recordUse(QStringLiteral("fire"));
        ranking = model.ranking();
        QCOMPARE(ranking.at(0).name, QStringLiteral("fire"));
        QCOMPARE(ranking.at(0).count, quint64(2));
        QVERIFY(qAbs(ranking.at(0).heat - 1.0) < 1e-12);

        Mattermost::ReactionUsageModel established;
        for (int i = 0; i < 64; ++i) {
            established.recordUse(QStringLiteral("+1"));
        }
        established.recordUse(QStringLiteral("rocket"));
        const auto establishedRanking = established.ranking();
        QCOMPARE(establishedRanking.at(0).name, QStringLiteral("rocket"));
        QCOMPARE(establishedRanking.at(1).count, quint64(64));
        QVERIFY(establishedRanking.at(1).heat < 1.0);
    }

    void establishedReactionCoolsMoreSlowly()
    {
        Mattermost::ReactionUsageModel model;
        for (int i = 0; i < 16; ++i) {
            model.recordUse(QStringLiteral("fire"));
        }
        model.recordUse(QStringLiteral("eyes"));
        model.recordUse(QStringLiteral("rocket"));

        const auto ranking = model.ranking();
        const auto find = [&ranking](const QString& name) {
            return std::find_if(ranking.cbegin(), ranking.cend(),
                                [&name](const Mattermost::ReactionUsageEntry& entry) {
                return entry.name == name;
            });
        };
        const auto fire = find(QStringLiteral("fire"));
        const auto eyes = find(QStringLiteral("eyes"));
        QVERIFY(fire != ranking.cend());
        QVERIFY(eyes != ranking.cend());
        QCOMPARE(fire->count, quint64(16));
        QCOMPARE(eyes->count, quint64(1));
        QVERIFY(fire->heat > eyes->heat);
    }

    void effectiveUsageCountsAgeInsteadOfGrowingForever()
    {
        Mattermost::ReactionUsageModel model;
        const quint64 threshold = Mattermost::ReactionUsageModel::CountAgingThreshold;

        for (quint64 i = 0; i < threshold - 1; ++i) {
            model.recordUse(QStringLiteral("fire"));
        }
        QCOMPARE(model.ranking().first().count, threshold - 1);

        // Reaching the threshold ages every familiarity count instead of
        // allowing one historical reaction to accumulate unbounded inertia.
        model.recordUse(QStringLiteral("fire"));
        QCOMPARE(model.ranking().first().count, threshold / 2);

        for (int i = 0; i < 4096; ++i) {
            model.recordUse(QStringLiteral("fire"));
        }
        QVERIFY(model.ranking().first().count < threshold);

        const double slowestCooling = Mattermost::ReactionUsageModel::coolingFactor(
            threshold - 1);
        const double longestHalfLife = std::log(0.5) / std::log(slowestCooling);
        QVERIFY(longestHalfLife < 16.0);
    }

    void persistedLargeUsageCountsAreAgedOnRestore()
    {
        Mattermost::ReactionUsageModel model;
        model.restore({
            Mattermost::ReactionUsageEntry {QStringLiteral("fire"), 4096, 0.8},
            Mattermost::ReactionUsageEntry {QStringLiteral("eyes"), 1024, 0.7},
        });

        const auto ranking = model.ranking();
        QCOMPARE(static_cast<int>(ranking.size()), 2);
        QCOMPARE(ranking.at(0).name, QStringLiteral("fire"));
        QCOMPARE(ranking.at(0).count, quint64(64));
        QVERIFY(qAbs(ranking.at(0).heat - 0.8) < 1e-12);
        QCOMPARE(ranking.at(1).name, QStringLiteral("eyes"));
        QCOMPARE(ranking.at(1).count, quint64(16));
        QVERIFY(qAbs(ranking.at(1).heat - 0.7) < 1e-12);
    }

    void popularityMapKeepsOnlyTenHottestEntries()
    {
        Mattermost::ReactionUsageModel model(10);
        for (int i = 0; i < 11; ++i) {
            model.recordUse(QStringLiteral("emoji_%1").arg(i));
        }

        QCOMPARE(model.size(), 10);
        QCOMPARE(model.topNames().first(), QStringLiteral("emoji_10"));
        QVERIFY(!model.topNames().contains(QStringLiteral("emoji_0")));
    }

    void reactionUsageStateRoundTrips()
    {
        Mattermost::ReactionUsageModel source;
        source.recordUse(QStringLiteral("fire"));
        source.recordUse(QStringLiteral("eyes"));
        source.recordUse(QStringLiteral("fire"));

        const QByteArray saved = Mattermost::serializeReactionUsage(source.ranking());
        Mattermost::ReactionUsageModel restored;
        restored.restore(Mattermost::deserializeReactionUsage(saved));

        const auto sourceRanking = source.ranking();
        const auto restoredRanking = restored.ranking();
        QCOMPARE(restoredRanking.size(), sourceRanking.size());
        const int count = static_cast<int>(sourceRanking.size());
        for (int i = 0; i < count; ++i) {
            QCOMPARE(restoredRanking.at(i).name, sourceRanking.at(i).name);
            QCOMPARE(restoredRanking.at(i).count, sourceRanking.at(i).count);
            QVERIFY(qAbs(restoredRanking.at(i).heat - sourceRanking.at(i).heat) < 1e-12);
        }
    }

    void seedsOnlyAnEmptyReactionRanking()
    {
        Mattermost::ReactionUsageModel model;
        model.seedIfEmpty(Mattermost::defaultReactionSeedNames());

        QCOMPARE(model.topNames(),
                 QStringList({QStringLiteral("+1"),
                              QStringLiteral("fire"),
                              QStringLiteral("heart")}));

        const auto seeded = model.ranking();
        QCOMPARE(seeded.size(), 3);
        for (const auto& entry : seeded) {
            QCOMPARE(entry.count, quint64(1));
            QVERIFY(qAbs(entry.heat - 0.5) < 1e-12);
        }

        model.recordUse(QStringLiteral("rocket"));
        QCOMPARE(model.topNames().first(), QStringLiteral("rocket"));

        model.seedIfEmpty({QStringLiteral("eyes")});
        QVERIFY(!model.topNames().contains(QStringLiteral("eyes")));
    }
};

QTEST_MAIN(EmojiDialogSupportTest)

#include "EmojiDialogSupportTest.moc"

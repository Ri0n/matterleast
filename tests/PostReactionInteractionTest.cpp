#include <algorithm>

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QPushButton>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QRegularExpression>
#include <QUrl>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/emoji/EmojiInfo.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendChannel.h"
#include "chat-area/ChatArea.h"
#include "chat-area/post/PostWidget.h"
#include "chat-area/post/ThreadSummaryWidget.h"
#include "chat-area/PostListWidget.h"
#include "chat-area/post/ReactionChipStyle.h"
#include "chat-area/post/reactions/PostReaction.h"
#include "chat-area/post/reactions/PostReactionList.h"

using namespace Mattermost;

class HoverTestList final : public PostListWidget
{
protected:
    QWidget* createItemWidget(int) override { return nullptr; }
};

class PostReactionInteractionTest : public QObject
{
    Q_OBJECT

private slots:
    void floatingToolbarStaysVisibleWhenPointerReturnsToItsPost()
    {
        Backend backend;
        Storage& storage = backend.getStorage();
        BackendPost post(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("aaaaabbbbbcccccdddddeeeee")},
                {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("message"), QStringLiteral("hello")},
            }, storage);

        HoverTestList list;
        list.resize(450, 350);
        list.show();
        auto* row = new PostWidget(backend, post, list.viewport(), nullptr, nullptr);
        row->resize(400, 140);
        row->move(0, 10);
        row->show();
        QApplication::processEvents();
        row->setHovered(true, true);

        auto* toolbar = list.viewport()->findChild<QFrame*>(
            QStringLiteral("postHoverActions"), Qt::FindDirectChildrenOnly);
        QVERIFY(toolbar);
        QVERIFY(toolbar->isVisible());

        // The popup reaction animation starts as the cursor enters its icon.
        // Leaving it for the body of the SAME post must not dismiss the whole
        // action bar after its 160ms leave grace period.
        QCursor::setPos(row->mapToGlobal(QPoint(65, 70)));
        QEvent leaveToolbar(QEvent::Leave);
        QCoreApplication::sendEvent(toolbar, &leaveToolbar);
        QTest::qWait(230);
        QVERIFY2(toolbar->isVisible(),
                 "Returning from the action bar to its post lost the toolbar");

        // Leaving both the action bar and the post still hides the toolbar.
        QCursor::setPos(list.viewport()->mapToGlobal(QPoint(440, 280)));
        QCoreApplication::sendEvent(toolbar, &leaveToolbar);
        QTRY_VERIFY_WITH_TIMEOUT(!toolbar->isVisible(), 1000);
    }

    void oneFloatingToolbarFollowsTheHoveredPost()
    {
        Backend backend;
        Storage& storage = backend.getStorage();
        BackendPost first(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("aaaaabbbbbcccccdddddeeeee")},
                {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("message"), QStringLiteral("first")},
            }, storage);
        BackendPost second(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("fffffggggghhhhhiiiiijjjjj")},
                {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
                {QStringLiteral("create_at"), 2},
                {QStringLiteral("message"), QStringLiteral("second")},
            }, storage);

        HoverTestList list;
        list.resize(450, 360);
        list.show();
        auto* one = new PostWidget(backend, first, list.viewport(), nullptr, nullptr);
        auto* two = new PostWidget(backend, second, list.viewport(), nullptr, nullptr);
        one->resize(400, 100);
        two->resize(400, 100);
        two->move(0, 120);
        one->show();
        two->show();
        QApplication::processEvents();

        // Materializing rows must not create any toolbar at all.
        QCOMPARE(list.viewport()->findChildren<QFrame*>(
            QStringLiteral("postHoverActions"), Qt::FindDirectChildrenOnly).size(), 0);

        one->setHovered(true, true);
        auto bars = list.viewport()->findChildren<QFrame*>(
            QStringLiteral("postHoverActions"), Qt::FindDirectChildrenOnly);
        QCOMPARE(bars.size(), 1);
        QFrame* bar = bars.first();
        QVERIFY(bar->isVisible());
        auto* reaction = bar->findChild<QPushButton*>();
        QVERIFY(reaction);
        QCOMPARE(reaction->property("matterleastPostOwner").value<QObject*>(),
                 static_cast<QObject*>(one));

        // Simulate a lost Leave on the previous row: physically there is
        // still just one toolbar, and its actions now belong to the new post.
        two->setHovered(true, true);
        QCOMPARE(list.viewport()->findChildren<QFrame*>(
            QStringLiteral("postHoverActions"), Qt::FindDirectChildrenOnly).size(), 1);
        QCOMPARE(reaction->property("matterleastPostOwner").value<QObject*>(),
                 static_cast<QObject*>(two));
        QVERIFY(bar->isVisible());

        delete one; // Virtualization of a formerly hovered row.
        QVERIFY(bar->isVisible());
        delete two; // Virtualization of the active target.
        QApplication::processEvents();
        QVERIFY(!bar->isVisible());
        QCOMPARE(reaction->property("matterleastPostOwner").value<QObject*>(),
                 static_cast<QObject*>(nullptr));
    }

    void floatingToolbarIconsFollowApplicationPaletteWithoutRecreation()
    {
        struct PaletteRestore {
            QPalette original;
            ~PaletteRestore() { qApp->setPalette(original); }
        } restore {qApp->palette()};

        Backend backend;
        Storage& storage = backend.getStorage();
        BackendPost post(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("aaaaabbbbbcccccdddddeeeee")},
                {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("message"), QStringLiteral("hello")},
            }, storage);

        QPalette first = restore.original;
        first.setColor(QPalette::WindowText, Qt::white);
        qApp->setPalette(first);

        HoverTestList list;
        list.resize(450, 350);
        list.show();
        auto* row = new PostWidget(backend, post, list.viewport(), nullptr, nullptr);
        row->resize(400, 140);
        row->show();
        QApplication::processEvents();
        row->setHovered(true, true);

        auto* toolbar = list.viewport()->findChild<QFrame*>(
            QStringLiteral("postHoverActions"), Qt::FindDirectChildrenOnly);
        QVERIFY(toolbar);
        const auto buttons = toolbar->findChildren<QPushButton*>();
        QCOMPARE(buttons.size(), 4);
        QList<QImage> whiteIcons;
        for (auto* button : buttons) {
            const QImage icon = button->icon().pixmap(24, 24).toImage();
            QVERIFY(!icon.isNull());
            whiteIcons.append(icon);
        }

        QPalette second = restore.original;
        second.setColor(QPalette::WindowText, Qt::black);
        qApp->setPalette(second);
        QApplication::processEvents();

        QCOMPARE(list.viewport()->findChild<QFrame*>(
            QStringLiteral("postHoverActions"), Qt::FindDirectChildrenOnly), toolbar);
        for (int i = 0; i < buttons.size(); ++i) {
            const QImage icon = buttons.at(i)->icon().pixmap(24, 24).toImage();
            QVERIFY(!icon.isNull());
            QVERIFY2(icon != whiteIcons.at(i),
                     "The shared toolbar retained an icon tinted for the previous palette");
        }

        qApp->setPalette(first);
        QApplication::processEvents();
        for (int i = 0; i < buttons.size(); ++i) {
            QCOMPARE(buttons.at(i)->icon().pixmap(24, 24).toImage(),
                     whiteIcons.at(i));
        }
    }

    void threadSummaryAndEmojiReactionsShareThreePixelTopGap()
    {
        Backend backend;
        Storage& storage = backend.getStorage();
        BackendChannel channel(
            storage, QJsonObject {
                {QStringLiteral("id"), QStringLiteral("cccccccccccccccccccccccccc")},
                {QStringLiteral("type"), QStringLiteral("O")},
                {QStringLiteral("display_name"), QStringLiteral("Test channel")},
            });
        // A lightweight channel surface lets PostWidget create the
        // thread-only engagement footer from the root's reply count.
        ChatArea area(backend, channel, nullptr, nullptr, false);
        BackendPost root(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb")},
                {QStringLiteral("channel_id"), channel.id},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("reply_count"), 7},
                {QStringLiteral("message"), QStringLiteral("A root with replies")},
            },
            storage);

        PostWidget widget(backend, root, nullptr, &area, nullptr,
                          PostWidget::PresentationMode::ReadOnlySnapshot);
        auto* engagement = widget.findChild<QWidget*>(
            QStringLiteral("postEngagementRow"));
        QVERIFY(engagement);
        QVERIFY(engagement->layout());
        QVERIFY(widget.findChild<ThreadSummaryWidget*>());
        QVERIFY(!widget.findChild<PostReactionList*>());
        QCOMPARE(engagement->layout()->contentsMargins().top(), 3);

        const QString reactor = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaa");
        root.addReaction(reactor, QStringLiteral("eyes"));
        widget.updateReactions();
        QVERIFY(widget.findChild<PostReactionList*>());
        QCOMPARE(engagement->layout()->contentsMargins().top(), 3);

        // Removing replies leaves a reaction-only footer; removing the last
        // reaction then clears the inset along with the engagement content.
        root.reply_count = 0;
        widget.addThreadButton();
        QCOMPARE(engagement->layout()->contentsMargins().top(), 3);
        root.removeReaction(reactor, QStringLiteral("eyes"));
        widget.updateReactions();
        QCOMPARE(engagement->layout()->contentsMargins().top(), 0);
    }

    void reactionCountTracksInheritedChatFont()
    {
        Backend backend;
        PostReactionList list(backend);
        list.addReaction(QStringLiteral("eyes"), QString::fromUtf8("👀"),
                         BackendPostReaction {QStringLiteral("Alice")});

        QFont chatFont = list.font();
        chatFont.setPointSize(19);
        list.setFont(chatFont);
        QApplication::processEvents();

        auto* chip = list.findChild<PostReaction*>();
        QVERIFY(chip);
        auto* count = chip->findChild<QLabel*>(QStringLiteral("count"));
        QVERIFY(count);
        const QFont expected = ReactionChipStyle::countFont(chatFont);
        QCOMPARE(count->font().pointSizeF(), expected.pointSizeF());

        auto* emoji = chip->findChild<QLabel*>(QStringLiteral("emoji"));
        QVERIFY(emoji);
        const int expectedExtent = ReactionChipStyle::iconExtent(chatFont);
        const int expectedBoxExtent = ReactionChipStyle::iconBoxExtent(chatFont);
        QCOMPARE(emoji->size(), QSize(expectedBoxExtent, expectedBoxExtent));
        QCOMPARE(emoji->font().pointSizeF(), chatFont.pointSizeF());
        QVERIFY(expectedBoxExtent > expectedExtent);
        QCOMPARE(chip->minimumHeight(), ReactionChipStyle::chipHeight(chatFont));
        QVERIFY(list.minimumHeight() >= ReactionChipStyle::chipHeight(chatFont));
        QVERIFY2(list.minimumHeight() > 27,
                 "Reaction list must not retain the legacy 27px height cap");
    }

    void reactionEventsAreIdempotentAndUseUserIds()
    {
        Backend backend;
        Storage& storage = backend.getStorage();

        const QString userId = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaa");
        QJsonObject user {
            {QStringLiteral("id"), userId},
            {QStringLiteral("username"), QStringLiteral("alice")},
            {QStringLiteral("first_name"), QStringLiteral("Alice")},
        };
        BackendUser* loginUser = storage.addUser(user, true);
        QVERIFY(loginUser);
        loginUser->isLoginUser = true;

        QJsonArray reactionArray {
            QJsonObject {
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("emoji_name"), QStringLiteral("eyes")},
            },
        };
        QJsonObject metadata {
            {QStringLiteral("reactions"), reactionArray},
        };
        QJsonObject postJson {
            {QStringLiteral("id"), QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb")},
            {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
            {QStringLiteral("user_id"), userId},
            {QStringLiteral("create_at"), 1},
            {QStringLiteral("metadata"), metadata},
        };

        BackendPost post(postJson, storage);
        auto reaction = post.reactions.find(QStringLiteral("eyes"));
        QVERIFY(reaction != post.reactions.end());
        QCOMPARE(reaction->second, BackendPostReaction {userId});

        // A replayed reaction_added event is a repeated fact, not a toggle.
        post.addReaction(userId, QStringLiteral("eyes"));
        reaction = post.reactions.find(QStringLiteral("eyes"));
        QVERIFY(reaction != post.reactions.end());
        QCOMPARE(reaction->second, BackendPostReaction {userId});

        post.removeReaction(userId, QStringLiteral("eyes"));
        QVERIFY(post.reactions.find(QStringLiteral("eyes"))
                == post.reactions.end());
    }

    void serverEmojiNameRemainsReactionIdentity()
    {
        Backend backend;
        Storage& storage = backend.getStorage();

        const QString userId = QStringLiteral("zzzzzzzzzzzzzzzzzzzzzzzzzz");
        BackendUser* loginUser = storage.addUser(
            QJsonObject {
                {QStringLiteral("id"), userId},
                {QStringLiteral("username"), QStringLiteral("alias-tester")},
            },
            true);
        QVERIFY(loginUser);
        loginUser->isLoginUser = true;

        // The built-in registry intentionally aliases "thumbsup" to the same
        // presentation as "+1". That makes this a deterministic regression for
        // reaction identity being replaced by EmojiInfo's canonical name.
        const QString wireName = QStringLiteral("thumbsup");
        const auto presentation =
            backend.emojiRegistry().resolveByName(wireName);
        QVERIFY(presentation);
        QCOMPARE(presentation->name, QStringLiteral("+1"));
        QVERIFY(presentation->name != wireName);

        BackendPost post(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("yyyyyyyyyyyyyyyyyyyyyyyyyy")},
                {QStringLiteral("channel_id"), QStringLiteral("xxxxxxxxxxxxxxxxxxxxxxxxxx")},
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("message"), QStringLiteral("alias")},
                {QStringLiteral("metadata"),
                 QJsonObject {
                     {QStringLiteral("reactions"),
                      QJsonArray {
                          QJsonObject {
                              {QStringLiteral("user_id"), userId},
                              {QStringLiteral("emoji_name"), wireName},
                          },
                      }},
                 }},
            },
            storage);

        auto reaction = post.reactions.find(wireName);
        QVERIFY(reaction != post.reactions.end());
        QCOMPARE(reaction->second, BackendPostReaction {userId});
        QVERIFY(post.hasReaction(userId, wireName));
        QVERIFY(!post.hasReaction(userId, presentation->name));

        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        auto* chip = widget.findChild<PostReaction*>();
        QVERIFY(chip);
        QVERIFY2(chip->toolTip().startsWith(wireName + QStringLiteral("  ")),
                 qPrintable(chip->toolTip()));

        auto* emojiLabel = chip->findChild<QLabel*>(QStringLiteral("emoji"));
        QVERIFY(emojiLabel);
        QVERIFY(emojiLabel->text().contains(QString::fromUtf8("👍")));
    }

    void customReactionSurvivesUntilEmojiRegistration()
    {
        Backend backend;
        Storage& storage = backend.getStorage();

        const QString userId = QStringLiteral("dddddddddddddddddddddddddd");
        BackendUser* loginUser = storage.addUser(
            QJsonObject {
                {QStringLiteral("id"), userId},
                {QStringLiteral("username"), QStringLiteral("customtester")},
                {QStringLiteral("first_name"), QStringLiteral("Custom")},
            },
            true);
        QVERIFY(loginUser);
        loginUser->isLoginUser = true;

        const QString customName =
            QStringLiteral("matterleast_pending_reaction_regression");
        QVERIFY(!backend.emojiRegistry().resolveByName(customName));

        QJsonArray reactionArray {
            QJsonObject {
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("emoji_name"), QStringLiteral("eyes")},
            },
            QJsonObject {
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("emoji_name"), customName},
            },
        };
        BackendPost post(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("eeeeeeeeeeeeeeeeeeeeeeeeee")},
                {QStringLiteral("channel_id"), QStringLiteral("ffffffffffffffffffffffffff")},
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("message"), QStringLiteral("hello")},
                {QStringLiteral("metadata"),
                 QJsonObject {{QStringLiteral("reactions"), reactionArray}}},
            },
            storage);

        QCOMPARE(static_cast<int>(post.reactions.size()), 2);
        auto customReaction = post.reactions.find(customName);
        QVERIFY(customReaction != post.reactions.end());
        QCOMPARE(customReaction->second, BackendPostReaction {userId});
        QVERIFY(post.hasReaction(userId, customName));

        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        PostWidget secondWidget(backend, post, nullptr, nullptr, nullptr);
        auto* list = widget.findChild<PostReactionList*>();
        auto* secondList = secondWidget.findChild<PostReactionList*>();
        QVERIFY2(list,
                 "An unresolved custom reaction must still have a visible placeholder chip");
        QVERIFY(secondList);
        QCOMPARE(list->findChildren<PostReaction*>().size(), 2);
        QCOMPARE(secondList->findChildren<PostReaction*>().size(), 2);

        const auto customChipText = [&customName](PostReactionList* reactionList) {
            const auto chips = reactionList->findChildren<PostReaction*>();
            for (PostReaction* chip : chips) {
                auto* emoji = chip->findChild<QLabel*>(QStringLiteral("emoji"));
                if (emoji && emoji->text().contains(customName)) {
                    return emoji->text();
                }
            }
            return QString();
        };
        const QString unresolvedText =
            QStringLiteral(":") + customName + QLatin1Char(':');
        QCOMPARE(customChipText(list), unresolvedText);
        QCOMPARE(customChipText(secondList), unresolvedText);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString imagePath = dir.filePath(QStringLiteral("custom.png"));
        QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QVERIFY(image.save(imagePath));

        // Registration can happen later through either the eager /emoji load or
        // CustomEmojiService. The existing post/widget must adopt it in place.
        backend.emojiRegistry().addCustomEmoji(customName, imagePath);

        QVERIFY(backend.emojiRegistry().resolveByName(customName));
        customReaction = post.reactions.find(customName);
        QVERIFY(customReaction != post.reactions.end());
        QCOMPARE(customReaction->second, BackendPostReaction {userId});
        QVERIFY(post.hasReaction(userId, customName));

        QApplication::processEvents();
        list = widget.findChild<PostReactionList*>();
        secondList = secondWidget.findChild<PostReactionList*>();
        QVERIFY(list);
        QVERIFY(secondList);
        QCOMPARE(list->findChildren<PostReaction*>().size(), 2);
        QCOMPARE(secondList->findChildren<PostReaction*>().size(), 2);
        const auto imageChipText = [](PostReactionList* reactionList) {
            const auto chips = reactionList->findChildren<PostReaction*>();
            for (PostReaction* chip : chips) {
                auto* emoji = chip->findChild<QLabel*>(QStringLiteral("emoji"));
                if (emoji && emoji->text().contains(QStringLiteral("<img"))) {
                    return emoji->text();
                }
            }
            return QString();
        };
        const QString firstRendered = imageChipText(list);
        const QString secondRendered = imageChipText(secondList);
        QVERIFY(!firstRendered.isEmpty());
        QVERIFY(!secondRendered.isEmpty());
        QVERIFY2(!firstRendered.contains(QStringLiteral("width=\"1\""))
                     && !firstRendered.contains(QStringLiteral("height=\"1\"")),
                 qPrintable(firstRendered));
        QVERIFY2(!secondRendered.contains(QStringLiteral("width=\"1\""))
                     && !secondRendered.contains(QStringLiteral("height=\"1\"")),
                 qPrintable(secondRendered));
    }

    void largeCustomReactionUsesSmoothedImageSource()
    {
        Backend backend;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QImage image(64, 64, QImage::Format_ARGB32);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const int shade = (x % 2 == 0) ? 0 : 255;
                image.setPixelColor(x, y, QColor(shade, shade, shade));
            }
        }
        const QString source = directory.filePath(QStringLiteral("jagged.png"));
        QVERIFY(image.save(source, "PNG"));
        const QString name = QStringLiteral("smoothed_reaction_test");
        backend.emojiRegistry().addCustomEmoji(name, source);
        const auto presentation = backend.emojiRegistry().resolveByName(name);
        QVERIFY(presentation);

        PostReaction chip(backend, name, presentation->unicodeString,
                          BackendPostReaction {}, nullptr);
        const QLabel* label = chip.findChild<QLabel*>(QStringLiteral("emoji"));
        QVERIFY(label);

        // The reaction chip must use the smoothed PNG, not ask QLabel to
        // downsample the original 64px custom image itself.
        const QRegularExpression sourceExpression(
            QStringLiteral("src=\"([^\"]+)\""));
        const auto match = sourceExpression.match(label->text());
        QVERIFY(match.hasMatch());
        const QString rasterPath = QUrl(match.captured(1)).toLocalFile();
        QVERIFY(!rasterPath.isEmpty());
        QVERIFY(rasterPath != source);
        const QImage reduced(rasterPath);
        QVERIFY(!reduced.isNull());
        const int center = reduced.pixelColor(
            reduced.width() / 2, reduced.height() / 2).red();
        QVERIFY(center > 20 && center < 235);
    }

    void customReactionCanBeRemovedBeforeRegistration()
    {
        Storage storage;
        const QString userId = QStringLiteral("gggggggggggggggggggggggggg");
        const QString customName =
            QStringLiteral("matterleast_pending_reaction_remove_test");
        QVERIFY(!EmojiInfo::resolveBuiltInByName(customName));

        BackendPost post(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("hhhhhhhhhhhhhhhhhhhhhhhhhh")},
                {QStringLiteral("channel_id"), QStringLiteral("iiiiiiiiiiiiiiiiiiiiiiiiii")},
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("create_at"), 1},
            },
            storage);

        post.addReaction(userId, customName);
        QVERIFY(post.hasReaction(userId, customName));
        QVERIFY(post.reactions.find(customName) != post.reactions.end());

        post.removeReaction(userId, customName);
        QVERIFY(!post.hasReaction(userId, customName));
        QVERIFY(post.reactions.find(customName) == post.reactions.end());
    }

    void liveReactionUpdateCommitsPostGeometry()
    {
        Backend backend;
        Storage& storage = backend.getStorage();

        const QString userId = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaa");
        BackendUser* user = storage.addUser(
            QJsonObject {
                {QStringLiteral("id"), userId},
                {QStringLiteral("username"), QStringLiteral("alice")},
                {QStringLiteral("first_name"), QStringLiteral("Alice")},
            },
            true);
        QVERIFY(user);
        user->isLoginUser = true;

        BackendPost post(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb")},
                {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("message"), QStringLiteral("hello")},
            },
            storage);

        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        widget.resize(480, widget.sizeHint().height());
        widget.show();
        QApplication::processEvents();
        const int before = widget.sizeHint().height();

        QSignalSpy dimensions(&widget, &PostWidget::dimensionsChanged);
        post.addReaction(userId, QStringLiteral("eyes"));
        widget.updateReactions();

        QTRY_VERIFY_WITH_TIMEOUT(dimensions.count() > 0, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(widget.sizeHint().height() > before, 1000);

        // PostWidget publishes size hints; its LongList owner owns the physical
        // top-level row rect. Apply the owner's geometry commit before checking
        // that the reaction child is contained.
        widget.resize(widget.width(), widget.sizeHint().height());
        QApplication::processEvents();

        auto* list = widget.findChild<PostReactionList*>();
        QVERIFY(list);
        QVERIFY(list->height() >= list->minimumSizeHint().height());
        QVERIFY(widget.rect().contains(
            list->mapTo(&widget, list->rect().bottomRight())));
    }

    void secondChipIsOneClickableHitTarget()
    {
        Backend backend;
        PostReactionList list(backend);
        list.addReaction(QStringLiteral("thumbsup"), QString::fromUtf8("👍"),
                         BackendPostReaction {QStringLiteral("Alice")});
        list.addReaction(QStringLiteral("eyes"), QString::fromUtf8("👀"),
                         BackendPostReaction {QStringLiteral("Bob")});
        list.resize(220, 32);
        list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        QApplication::processEvents();

        auto chips = list.findChildren<PostReaction*>();
        QCOMPARE(chips.size(), 2);
        std::sort(chips.begin(), chips.end(), [](PostReaction* lhs, PostReaction* rhs) {
            return lhs->mapToGlobal(QPoint()).x() < rhs->mapToGlobal(QPoint()).x();
        });
        PostReaction* second = chips.at(1);
        const QPoint globalCenter = second->mapToGlobal(second->rect().center());
        QWidget* hit = QApplication::widgetAt(globalCenter);
        QCOMPARE(hit, static_cast<QWidget*>(second));

        QSignalSpy spy(&list, &PostReactionList::reactionClicked);
        QTest::mouseClick(hit, Qt::LeftButton);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("eyes"));
    }
};

QTEST_MAIN(PostReactionInteractionTest)
#include "PostReactionInteractionTest.moc"

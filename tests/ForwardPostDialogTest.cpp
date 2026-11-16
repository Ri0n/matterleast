#include <QtTest>

#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "chat-area/post/ForwardPostDialog.h"

using namespace Mattermost;

class ForwardPostDialogTest : public QObject
{
    Q_OBJECT

private:
    static BackendChannel* addChannel(Backend& backend, const QString& id,
                                      const QString& type,
                                      const QString& name)
    {
        Storage& storage = backend.getStorage();
        BackendTeam* team = storage.getTeamById(QStringLiteral("team"));
        if (!team) {
            team = storage.addTeam(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("team")},
                {QStringLiteral("name"), QStringLiteral("team")},
                {QStringLiteral("display_name"), QStringLiteral("Team")},
            });
        }
        return storage.addTeamChannel(*team, QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("team_id"), QStringLiteral("team")},
            {QStringLiteral("type"), type},
            {QStringLiteral("name"), name},
            {QStringLiteral("display_name"), name},
        });
    }

    static BackendPost sourcePost(Backend& backend, const QString& channelId)
    {
        return BackendPost(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("original-post")},
            {QStringLiteral("channel_id"), channelId},
            {QStringLiteral("message"), QStringLiteral("Original content")},
        }, backend.getStorage());
    }

private slots:
    void initTestCase()
    {
        NetworkRequest::setHost(QStringLiteral("https://mattermost.example.test/"));
    }

    void publicOriginListsKnownDestinationsAndFilters()
    {
        Backend backend;
        QVERIFY(addChannel(backend, QStringLiteral("source"), QStringLiteral("O"),
                           QStringLiteral("source")));
        QVERIFY(addChannel(backend, QStringLiteral("destination"), QStringLiteral("O"),
                           QStringLiteral("destination")));
        QVERIFY(addChannel(backend, QStringLiteral("private"), QStringLiteral("P"),
                           QStringLiteral("private")));
        ForwardPostDialog dialog(backend, sourcePost(backend, QStringLiteral("source")));
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("forwardDestinations"));
        auto* filter = dialog.findChild<QLineEdit*>(QStringLiteral("forwardDestinationFilter"));
        auto* submit = dialog.findChild<QPushButton*>(QStringLiteral("forwardSubmit"));
        QVERIFY(list && filter && submit);
        QCOMPARE(list->count(), 3);
        QVERIFY(submit->isEnabled());
        filter->setText(QStringLiteral("destination"));
        int visible = 0;
        for (int i = 0; i < list->count(); ++i) {
            if (!list->item(i)->isHidden()) ++visible;
        }
        QCOMPARE(visible, 1);
        filter->setText(QStringLiteral("not-there"));
        QVERIFY(!submit->isEnabled());
    }

    void privateOriginOnlyOffersItsOwnChannel()
    {
        Backend backend;
        QVERIFY(addChannel(backend, QStringLiteral("private"), QStringLiteral("P"),
                           QStringLiteral("private")));
        QVERIFY(addChannel(backend, QStringLiteral("other"), QStringLiteral("O"),
                           QStringLiteral("other")));
        ForwardPostDialog dialog(backend, sourcePost(backend, QStringLiteral("private")));
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("forwardDestinations"));
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("private"));
        QVERIFY(!dialog.findChild<QLineEdit*>(QStringLiteral("forwardDestinationFilter")));
        QVERIFY(dialog.findChild<QWidget*>(QStringLiteral("forwardPrivacyNotice")));
        QVERIFY(dialog.findChild<QPlainTextEdit*>(QStringLiteral("forwardComment")));
    }
};

QTEST_MAIN(ForwardPostDialogTest)
#include "ForwardPostDialogTest.moc"

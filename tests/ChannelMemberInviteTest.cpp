#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/WebSocketEventHandler.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "chat-area/ChatArea.h"
#include "chat-area/ChatLogWidget.h"
#include "chat-area/post/PostWidget.h"
#include "channel-tree-dialogs/UserSearchDialog.h"
#include "channel-tree-dialogs/ViewChannelMembersListDialog.h"

using namespace Mattermost;

namespace {
QJsonObject user(const QString& id)
{
    return {{"id", id}, {"username", id}, {"first_name", id},
            {"roles", "system_user custom_system"}};
}

class MemberServer : public QTcpServer
{
public:
    QString grantingRole = QStringLiteral("custom_channel");
    QString permission = QStringLiteral("manage_private_channel_members");
    bool failRoles = false;
    bool deletedRole = false;
    bool malformedMember = false;
    QString failUser;
    QStringList additions;
    QStringList paths;
    QJsonObject search;
    QVector<QJsonObject> searches;
    QSet<QString> existingMembers;
    QJsonArray requestedRoles;
    int count = 1;

    MemberServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    if (socket->property("answered").toBool()) return;
                    const QByteArray data = socket->property("input").toByteArray() + socket->readAll();
                    socket->setProperty("input", data);
                    const int headerEnd = data.indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    int length = 0;
                    for (const auto& line : data.left(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                        }
                    }
                    if (data.size() < headerEnd + 4 + length) return;
                    socket->setProperty("answered", true);
                    const auto request = data.left(data.indexOf('\r')).split(' ');
                    const QString path = QUrl(QString::fromUtf8(request.value(1))).path();
                    paths.append(path);
                    const auto input = QJsonDocument::fromJson(data.mid(headerEnd + 4, length));
                    int status = 200;
                    QJsonDocument response(QJsonObject {});
                    if (path.endsWith("/roles/names")) {
                        requestedRoles = input.array();
                        QJsonArray roles;
                        for (const auto& name : requestedRoles) {
                            roles.append(QJsonObject {
                                {"name", name}, {"delete_at", deletedRole ? 1 : 0},
                                {"permissions", name.toString() == grantingRole
                                    ? QJsonArray {permission} : QJsonArray {}}
                            });
                        }
                        response = QJsonDocument(roles);
                        if (failRoles) status = 403;
                    } else if (path.endsWith("/members/self")) {
                        response = QJsonDocument(QJsonObject {
                            {"user_id", "self"}, {"channel_id", "channel"},
                            {"team_id", "team"}, {"scheme_admin", true},
                            {"roles", path.contains("/teams/") ? "custom_team" : "custom_channel"}
                        });
                    } else if (path.endsWith("/users/search")) {
                        search = input.object();
                        searches.push_back(search);
                        QJsonArray users;
                        for (int index = 0; index < 9; ++index) {
                            const QString id = QStringLiteral("person%1").arg(index, 2, 10, QLatin1Char('0'));
                            if (search.contains("in_channel_id") && !existingMembers.contains(id)) continue;
                            if (search.contains("not_in_channel_id") && existingMembers.contains(id)) continue;
                            users.append(user(id));
                        }
                        response = QJsonDocument(users);
                    } else if (path.endsWith("/members") && request.value(0) == "POST") {
                        const QString id = input.object().value("user_id").toString();
                        additions.append(id);
                        if (id == failUser) {
                            status = 403;
                            response = QJsonDocument(QJsonObject {{"message", "Permission denied for this person"}});
                        } else {
                            ++count;
                            existingMembers.insert(id);
                            response = QJsonDocument(QJsonObject {
                                {"user_id", id}, {"channel_id", malformedMember ? "wrong" : "channel"},
                                {"roles", "channel_user"}
                            });
                        }
                    } else if (path.endsWith("/stats")) {
                        response = QJsonDocument(QJsonObject {{"member_count", count}});
                    } else if (path.endsWith("/members")) {
                        response = QJsonDocument(QJsonArray {QJsonObject {{"user_id", "self"}}});
                    } else if (path.endsWith("/users/ids")) {
                        QJsonArray users;
                        for (const auto& id : input.array()) users.append(user(id.toString()));
                        response = QJsonDocument(users);
                    } else if (path.contains("/status/ids")) {
                        response = QJsonDocument(QJsonArray {});
                    }
                    const auto body = response.toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 " + QByteArray::number(status)
                        + (status == 200 ? " OK\r\n" : " Forbidden\r\n")
                        + "Content-Type: application/json\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    bool start()
    {
        if (!listen(QHostAddress::LocalHost)) return false;
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(serverPort()));
        return true;
    }
};

BackendChannel* makeChannel(Backend& backend, const QString& type = QStringLiteral("P"))
{
    auto& storage = backend.getStorage();
    storage.addUser(user(QStringLiteral("self")), true);
    auto* team = storage.addTeam(QJsonObject {{"id", "team"}, {"name", "team"}});
    return storage.addTeamChannel(*team, QJsonObject {
        {"id", "channel"}, {"team_id", "team"}, {"name", "test"},
        {"display_name", "Test Channel"}, {"type", type}
    });
}
} // namespace

class ChannelMemberInviteTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void effectivePermission_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("role");
        QTest::addColumn<QString>("permission");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("private-channel-role") << "P" << "custom_channel" << "manage_private_channel_members" << true;
        QTest::newRow("public-channel-role") << "O" << "custom_channel" << "manage_public_channel_members" << true;
        QTest::newRow("team-role") << "P" << "custom_team" << "manage_private_channel_members" << true;
        QTest::newRow("system-role") << "P" << "custom_system" << "manage_private_channel_members" << true;
        QTest::newRow("admin-flag-does-not-grant") << "P" << "unassigned_role" << "manage_private_channel_members" << false;
        QTest::newRow("public-permission-is-not-private") << "P" << "custom_channel" << "manage_public_channel_members" << false;
        QTest::newRow("direct") << "D" << "custom_system" << "manage_private_channel_members" << false;
        QTest::newRow("group") << "G" << "custom_system" << "manage_private_channel_members" << false;
    }
    void effectivePermission()
    {
        QFETCH(QString, type);
        QFETCH(QString, role);
        QFETCH(QString, permission);
        QFETCH(bool, allowed);
        MemberServer server;
        QVERIFY(server.start());
        server.grantingRole = role;
        server.permission = permission;
        Backend backend;
        auto* channel = makeChannel(backend);
        QVERIFY(channel);
        channel->type = type == "D" ? BackendChannel::directChannel
            : type == "G" ? BackendChannel::groupChannel
            : type == "O" ? BackendChannel::publicChannel : BackendChannel::privateChannel;
        bool done = false;
        bool result = !allowed;
        backend.canAddChannelMembers(*channel, [&](bool value) { result = value; done = true; });
        QTRY_VERIFY(done);
        QCOMPARE(result, allowed);
        if (type == "P" || type == "O") {
            QVERIFY(server.requestedRoles.contains("custom_system"));
            QVERIFY(server.requestedRoles.contains("custom_team"));
            QVERIFY(server.requestedRoles.contains("custom_channel"));
        } else {
            QVERIFY(server.paths.isEmpty());
        }
    }

    void permissionFailuresAndArchivedChannelsHideTheAction_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("role-request-denied") << 0;
        QTest::newRow("deleted-role") << 1;
        QTest::newRow("archived") << 2;
    }
    void permissionFailuresAndArchivedChannelsHideTheAction()
    {
        QFETCH(int, failure);
        MemberServer server;
        QVERIFY(server.start());
        server.failRoles = failure == 0;
        server.deletedRole = failure == 1;
        Backend backend;
        auto* channel = makeChannel(backend);
        if (failure == 2) channel->delete_at = 1;
        bool done = false;
        backend.canAddChannelMembers(*channel, [&](bool allowed) { QVERIFY(!allowed); done = true; });
        QTRY_VERIFY(done);
        ViewChannelMembersListDialog dialog(backend, *channel, nullptr);
        auto* add = dialog.findChild<QPushButton*>("addChannelMembersButton");
        QVERIFY(add);
        QTest::qWait(100);
        QVERIFY(add->isHidden());
    }

    void memberDialogOffersSharedPickerWhenAllowed()
    {
        MemberServer server;
        QVERIFY(server.start());
        Backend backend;
        auto* channel = makeChannel(backend);
        ViewChannelMembersListDialog dialog(backend, *channel, nullptr);
        auto* add = dialog.findChild<QPushButton*>("addChannelMembersButton");
        QVERIFY(add);
        QTRY_VERIFY(!add->isHidden());
        add->click();
        auto* picker = dynamic_cast<UserSearchDialog*>(dialog.findChild<QDialog*>());
        QVERIFY(picker);
        QVERIFY(!add->isEnabled());
        QVERIFY(picker->findChild<QWidget*>("conversationParticipants"));
        QVERIFY(picker->findChild<QPushButton*>("addSelectedMembersButton"));
        QVERIFY(!picker->findChild<QPushButton*>("newConversationButton"));
        picker->reject();
    }

    void existingMembersOutsideTheCacheRemainVisibleAndCannotBeAdded()
    {
        MemberServer server;
        QVERIFY(server.start());
        server.existingMembers.insert(QStringLiteral("person03"));
        Backend backend;
        auto* channel = makeChannel(backend);
        QVERIFY(channel->members.isEmpty());
        QPointer<UserSearchDialog> picker = UserSearchDialog::showChannelMemberPicker(backend, *channel);
        auto* edit = picker->findChild<QLineEdit*>("filterLineEdit");
        auto* table = picker->findChild<QTableWidget*>("tableWidget");
        auto* add = picker->findChild<QPushButton*>("addSelectedMembersButton");
        QTest::keyClicks(edit, "person");
        QTRY_COMPARE(table->rowCount(), 9);
        QCOMPARE(table->columnCount(), 3);
        QCOMPARE(table->item(3, 0)->text(), QStringLiteral("person03"));
        QCOMPARE(table->item(3, 2)->text(), QStringLiteral("Already in channel"));
        table->selectRow(3);
        emit table->cellDoubleClicked(3, 0);
        QVERIFY(!add->isEnabled());
        picker->accept();
        QVERIFY(server.additions.isEmpty());
        QVERIFY(picker);
        table->selectRow(4);
        QVERIFY(add->isEnabled());
        QCOMPARE(table->item(4, 2)->text(), QStringLiteral("Not in channel"));
        picker->reject();
    }

    void multipleAdditionsRetainFailuresAndRetryOnlyRemainingPeople()
    {
        MemberServer server;
        QVERIFY(server.start());
        server.failUser = QStringLiteral("person03");
        Backend backend;
        auto* channel = makeChannel(backend);
        QPointer<UserSearchDialog> picker = UserSearchDialog::showChannelMemberPicker(backend, *channel);
        QSignalSpy accepted(picker, &QDialog::accepted);
        auto* edit = picker->findChild<QLineEdit*>("filterLineEdit");
        auto* table = picker->findChild<QTableWidget*>("tableWidget");
        auto* add = picker->findChild<QPushButton*>("addSelectedMembersButton");
        QTest::keyClicks(edit, "person");
        QTRY_COMPARE(table->rowCount(), 9);
        QCOMPARE(server.search.value("team_id").toString(), QStringLiteral("team"));
        QCOMPARE(server.searches.size(), 2);
        bool searchedMembers = false;
        bool searchedNonMembers = false;
        for (const auto& search : server.searches) {
            searchedMembers |= search.value("in_channel_id").toString() == "channel";
            searchedNonMembers |= search.value("not_in_channel_id").toString() == "channel";
        }
        QVERIFY(searchedMembers);
        QVERIFY(searchedNonMembers);
        for (int row = 0; row < 9; ++row) {
            table->selectRow(row);
            emit table->cellDoubleClicked(row, 0);
        }
        QVERIFY(add->isEnabled()); // Channels do not inherit the GM size limit.
        add->click();
        auto* error = picker->findChild<QLabel*>("channelMemberError");
        QTRY_VERIFY(!error->isHidden());
        QVERIFY(error->text().contains("Permission denied"));
        QCOMPARE(accepted.size(), 0);
        QCOMPARE(channel->members.size(), 3);
        QCOMPARE(server.additions.size(), 4);
        QCOMPARE(table->rowCount(), 9);
        QCOMPARE(table->columnCount(), 3);
        for (int row = 0; row < 9; ++row) {
            QCOMPARE(table->item(row, 2)->text(), row < 3
                ? QStringLiteral("Already in channel") : QStringLiteral("Not in channel"));
            QCOMPARE(bool(table->item(row, 0)->flags() & Qt::ItemIsSelectable), row >= 3);
        }
        server.failUser.clear();
        add->click();
        QTRY_COMPARE(accepted.size(), 1);
        QCOMPARE(channel->members.size(), 9);
        QCOMPARE(server.additions.size(), 10);
        QCOMPARE(server.additions.count("person00"), 1);
        QCOMPARE(server.additions.count("person03"), 2);
        QVERIFY(!server.paths.contains("/api/v4/channels/group"));
        QVERIFY(!server.paths.contains("/api/v4/channels/direct"));
    }

    void conversationPickerKeepsItsOwnActionsAndSizeLimit()
    {
        MemberServer server;
        QVERIFY(server.start());
        Backend backend;
        makeChannel(backend);
        QPointer<UserSearchDialog> picker = UserSearchDialog::showConversationPicker(backend);
        auto* edit = picker->findChild<QLineEdit*>("filterLineEdit");
        auto* table = picker->findChild<QTableWidget*>("tableWidget");
        auto* create = picker->findChild<QPushButton*>("newConversationButton");
        QVERIFY(create);
        QVERIFY(!picker->findChild<QPushButton*>("addSelectedMembersButton"));
        QTest::keyClicks(edit, "person");
        QTRY_COMPARE(table->rowCount(), 9);
        for (int row = 0; row < 8; ++row) {
            table->selectRow(row);
            emit table->cellDoubleClicked(row, 0);
        }
        QVERIFY(!create->isEnabled());
        QVERIFY(!create->toolTip().isEmpty());
        QVERIFY(server.additions.isEmpty());
        picker->reject();
    }

    void customStatusColumnUpdatesAndEscapesUserText()
    {
        MemberServer server;
        QVERIFY(server.start());
        Backend backend;
        auto* channel = makeChannel(backend);
        ViewChannelMembersListDialog dialog(backend, *channel, nullptr);
        dialog.show();
        QTRY_VERIFY(dialog.findChild<QLabel*>("memberCustomStatus"));
        auto* label = dialog.findChild<QLabel*>("memberCustomStatus");
        auto profile = user(QStringLiteral("self"));
        const auto setStatus = [&](const QJsonObject& status) {
            profile.insert("props", QJsonObject {{"customStatus",
                QString::fromUtf8(QJsonDocument(status).toJson(QJsonDocument::Compact))}});
            backend.getStorage().addUser(profile);
        };
        setStatus(QJsonObject {{"emoji", "coffee"}, {"text", "<focus>"}});
        QVERIFY(label->text().contains("&lt;focus&gt;"));
        QVERIFY(!label->text().contains(":coffee:"));
        QCOMPARE(label->toolTip(), QStringLiteral("<focus>"));
        setStatus(QJsonObject {{"text", "Updated"}});
        QCOMPARE(label->text(), QStringLiteral("Updated"));
        setStatus(QJsonObject {{"text", "Expired"}, {"expiresAt", "2020-01-01T00:00:00.000Z"}});
        QVERIFY(label->text().isEmpty());
        setStatus(QJsonObject {});
        QVERIFY(label->text().isEmpty());
    }

    void outOfChannelMentionIsTransientAndInvitable()
    {
        MemberServer server;
        QVERIFY(server.start());
        Backend backend;
        BackendChannel* channel = makeChannel(backend);
        QVERIFY(channel);
        BackendPost* original = channel->addPost(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaa")},
            {QStringLiteral("user_id"), QStringLiteral("self")},
            {QStringLiteral("channel_id"), channel->id},
            {QStringLiteral("create_at"), 10000},
            {QStringLiteral("message"), QStringLiteral("Hello @person00")},
        });
        QVERIFY(original);
        QVERIFY(original->isOwnPost());

        const QJsonObject ephemeral {
            {QStringLiteral("id"), QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb")},
            {QStringLiteral("type"), QStringLiteral("system_ephemeral")},
            {QStringLiteral("channel_id"), channel->id},
            {QStringLiteral("root_id"), QString()},
            {QStringLiteral("create_at"), 10001},
            {QStringLiteral("message"), QStringLiteral("User not in channel")},
            {QStringLiteral("props"), QJsonObject {
                {QStringLiteral("add_channel_member"), QJsonObject {
                    {QStringLiteral("post_id"), QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb")},
                    {QStringLiteral("not_in_channel_user_ids"), QJsonArray {
                        QStringLiteral("person00")}},
                    {QStringLiteral("not_in_channel_usernames"), QJsonArray {
                        QStringLiteral("person00")}},
                    {QStringLiteral("not_in_groups_user_ids"), QJsonArray {}},
                    {QStringLiteral("not_in_groups_usernames"), QJsonArray {}},
                }},
            }},
        };

        // The websocket handler must hand structured guidance to the UI,
        // rather than inserting the ephemeral post in the channel model.
        QSignalSpy ephemeralSpy(&backend, &Backend::onEphemeralPost);
        WebSocketEventHandler handler(backend);
        handler.handleEphemeralMessage(QJsonObject {
            {QStringLiteral("post"), QString::fromUtf8(
                QJsonDocument(ephemeral).toJson(QJsonDocument::Compact))},
        });
        QCOMPARE(ephemeralSpy.count(), 1);
        QCOMPARE(channel->postIdToPost.size(), 1);

        ChatArea area(backend, *channel, static_cast<ChannelItem*>(nullptr),
                      nullptr, false);
        area.resize(700, 450);
        area.show();
        auto* list = area.findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QVERIFY(list);
        PostWidget widget(backend, *original, list->viewport(), &area, nullptr);
        widget.show();
        widget.setEphemeralMentionNotice(
            ephemeralSpy.at(0).at(0).toJsonObject());
        // Replayed websocket data must not duplicate the attached banner.
        widget.setEphemeralMentionNotice(ephemeral);
        QCOMPARE(widget.findChildren<QWidget*>(
            QStringLiteral("ephemeralMentionNotice")).size(), 1);
        auto* label = widget.findChild<QLabel*>(
            QStringLiteral("ephemeralMentionText"));
        QVERIFY(label);
        QVERIFY(label->text().contains(QStringLiteral("@person00")));
        auto* button = widget.findChild<QPushButton*>(
            QStringLiteral("inviteMentionedUsersButton"));
        QVERIFY(button);
        QTRY_VERIFY(button->isEnabled());

        QSignalSpy dismissed(&widget, &PostWidget::ephemeralInviteDismissed);
        button->click();
        QTRY_COMPARE(dismissed.count(), 1);
        QCOMPARE(dismissed.at(0).at(0).toULongLong(), qulonglong(10000));
        QCOMPARE(server.additions, QStringList {QStringLiteral("person00")});
        QVERIFY(widget.findChild<QWidget*>(
            QStringLiteral("ephemeralMentionNotice"))->isHidden());
    }

    void groupConstrainedMentionHasNoInviteAction()
    {
        MemberServer server;
        QVERIFY(server.start());
        Backend backend;
        BackendChannel* channel = makeChannel(backend);
        QVERIFY(channel);
        BackendPost* original = channel->addPost(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("cccccccccccccccccccccccccc")},
            {QStringLiteral("user_id"), QStringLiteral("self")},
            {QStringLiteral("channel_id"), channel->id},
            {QStringLiteral("create_at"), 20000},
            {QStringLiteral("message"), QStringLiteral("Hello @person01")},
        });
        QVERIFY(original);
        ChatArea area(backend, *channel, static_cast<ChannelItem*>(nullptr),
                      nullptr, false);
        auto* list = area.findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QVERIFY(list);
        PostWidget widget(backend, *original, list->viewport(), &area, nullptr);
        const QJsonObject ephemeral {
            {QStringLiteral("id"), QStringLiteral("dddddddddddddddddddddddddd")},
            {QStringLiteral("type"), QStringLiteral("system_ephemeral")},
            {QStringLiteral("channel_id"), channel->id},
            {QStringLiteral("root_id"), QString()},
            {QStringLiteral("create_at"), 20001},
            {QStringLiteral("props"), QJsonObject {
                {QStringLiteral("add_channel_member"), QJsonObject {
                    {QStringLiteral("post_id"), QStringLiteral("dddddddddddddddddddddddddd")},
                    {QStringLiteral("not_in_channel_user_ids"), QJsonArray {}},
                    {QStringLiteral("not_in_channel_usernames"), QJsonArray {}},
                    {QStringLiteral("not_in_groups_user_ids"), QJsonArray {
                        QStringLiteral("person01")}},
                    {QStringLiteral("not_in_groups_usernames"), QJsonArray {
                        QStringLiteral("person01")}},
                }},
            }},
        };
        widget.setEphemeralMentionNotice(ephemeral);
        auto* label = widget.findChild<QLabel*>(
            QStringLiteral("ephemeralMentionText"));
        QVERIFY(label);
        QVERIFY(label->text().contains(QStringLiteral("@person01")));
        QVERIFY(label->text().contains(QStringLiteral("group restrictions")));
        auto* button = widget.findChild<QPushButton*>(
            QStringLiteral("inviteMentionedUsersButton"));
        QVERIFY(button);
        QVERIFY(button->isHidden());
        QVERIFY(server.additions.isEmpty());
    }

    void malformedSuccessDoesNotAdmitMembership()
    {
        MemberServer server;
        QVERIFY(server.start());
        server.malformedMember = true;
        Backend backend;
        auto* channel = makeChannel(backend);
        bool done = false;
        backend.addChannelMember(*channel, "person00", [&](bool success, const QString& error) {
            QVERIFY(!success);
            QVERIFY(!error.isEmpty());
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(channel->members.isEmpty());
    }
};

QTEST_MAIN(ChannelMemberInviteTest)
#include "ChannelMemberInviteTest.moc"

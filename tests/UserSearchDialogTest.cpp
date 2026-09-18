#include <set>

#include <QJsonObject>
#include <QLineEdit>
#include <QTableWidget>
#include <QtTest>

#include "backend/Backend.h"
#include "backend/types/BackendUser.h"
#include "channel-tree-dialogs/UserListDialog.h"
#include "channel-tree-dialogs/UserSearchDialog.h"

using namespace Mattermost;

namespace {

QJsonObject userJson(const QString& id,
                     const QString& username,
                     const QString& firstName,
                     const QString& lastName)
{
    return QJsonObject {
        {QStringLiteral("id"), id},
        {QStringLiteral("create_at"), 1},
        {QStringLiteral("update_at"), 1},
        {QStringLiteral("delete_at"), 0},
        {QStringLiteral("username"), username},
        {QStringLiteral("email"), username + QStringLiteral("@example.invalid")},
        {QStringLiteral("first_name"), firstName},
        {QStringLiteral("last_name"), lastName},
        {QStringLiteral("nickname"), QString()},
        {QStringLiteral("notify_props"), QJsonObject {}},
        {QStringLiteral("props"), QJsonObject {}},
        {QStringLiteral("timezone"), QJsonObject {}},
    };
}

FilterListDialogConfig dialogConfig()
{
    FilterListDialogConfig config;
    config.title = QStringLiteral("People");
    config.description = QStringLiteral("People");
    config.filterLabelText = QStringLiteral("Search");
    config.buttons = QDialogButtonBox::NoButton;
    return config;
}

class TestUserListDialog final : public UserListDialog
{
public:
    explicit TestUserListDialog(QWidget* parent = nullptr)
        : UserListDialog(parent)
    {
    }

    using UserListDialog::create;
};

} // namespace

class UserSearchDialogTest : public QObject
{
    Q_OBJECT

private slots:
    void rebuildClearsHiddenRowState()
    {
        BackendUser alice(userJson(
            QStringLiteral("alice-id"),
            QStringLiteral("alice"),
            QStringLiteral("Alice"),
            QStringLiteral("Example")));
        BackendUser bob(userJson(
            QStringLiteral("bob-id"),
            QStringLiteral("bob"),
            QStringLiteral("Bob"),
            QStringLiteral("Example")));

        std::set<UserListEntry> entries {
            UserListEntry(&alice),
            UserListEntry(&bob),
        };

        TestUserListDialog dialog;
        dialog.create(dialogConfig(), entries,
                      {QStringLiteral("Full Name"), QStringLiteral("Status")});

        auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("tableWidget"));
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 2);

        table->hideRow(0);
        QVERIFY(table->isRowHidden(0));

        dialog.create(dialogConfig(), entries,
                      {QStringLiteral("Full Name"), QStringLiteral("Status")});

        QCOMPARE(table->rowCount(), 2);
        QVERIFY(!table->isRowHidden(0));
        QVERIFY(!table->isRowHidden(1));
    }

    void usernameMatchSurvivesFourthCharacter()
    {
        Backend backend;
        auto& storage = backend.getStorage();
        storage.addUser(
            userJson(QStringLiteral("self-id"),
                     QStringLiteral("self"),
                     QStringLiteral("Self"),
                     QStringLiteral("User")),
            true);
        BackendUser* remote = storage.addUser(
            userJson(QStringLiteral("remote-id"),
                     QStringLiteral("rionuser"),
                     QStringLiteral("Alice"),
                     QStringLiteral("Example")));
        QVERIFY(remote);

        // UserSearchDialog only needs the key to treat this as an existing DM
        // while rendering; the channel pointer is not dereferenced during search.
        storage.directChannelsByUser.insert(remote->id, nullptr);

        UserSearchOptions options;
        UserSearchDialog dialog(backend, dialogConfig(), options);

        auto* edit = dialog.findChild<QLineEdit*>(QStringLiteral("filterLineEdit"));
        auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("tableWidget"));
        QVERIFY(edit);
        QVERIFY(table);

        // The query matches username/email but not the visible "Alice Example"
        // cell. The old generic FilterListDialog path hid the row by column 0.
        QTest::keyClicks(edit, QStringLiteral("rion"));

        QCOMPARE(edit->text(), QStringLiteral("rion"));
        QCOMPARE(table->rowCount(), 1);
        QVERIFY(!table->isRowHidden(0));
        QVERIFY(table->item(0, 0));
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("Alice Example"));
    }
};

QTEST_MAIN(UserSearchDialogTest)
#include "UserSearchDialogTest.moc"

#include <QtTest>

#include "workspaces/AccountListModel.h"

using namespace Mattermost;

class AccountListModelTest final : public QObject
{
    Q_OBJECT
private slots:
    void statusesUpdateRolesWithoutRecreatingRows()
    {
        AccountListModel model;
        const QVector<AccountListModel::Entry> initial {
            {QStringLiteral("a"), QStringLiteral("Work"),
             QStringLiteral("work.example.org"), AccountConnectionState::Connected},
            {QStringLiteral("b"), QStringLiteral("Home"),
             QStringLiteral("home.example.org"), AccountConnectionState::Connecting}
        };
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy added(&model, &QAbstractItemModel::rowsInserted);
        model.setEntries(initial, QStringLiteral("a"));
        QCOMPARE(added.count(), 2);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0, 0), AccountListModel::AccountIdRole).toString(),
                 QStringLiteral("a"));
        QCOMPARE(model.data(model.index(0, 0), AccountListModel::SelectedRole).toBool(),
                 true);
        QCOMPARE(model.roleNames().value(AccountListModel::StatusRole),
                 QByteArrayLiteral("status"));

        auto changed = initial;
        changed[1].state = AccountConnectionState::AuthenticationRequired;
        const int baseline = changes.count();
        model.setEntries(changed, QStringLiteral("a"));
        QCOMPARE(added.count(), 2);
        QCOMPARE(resets.count(), 0);
        QCOMPARE(changes.count(), baseline + 1);
        const QList<QVariant> args = changes.last();
        QCOMPARE(args.at(0).value<QModelIndex>().row(), 1);
        const QVector<int> roles = args.at(2).value<QVector<int>>();
        QVERIFY(roles.contains(AccountListModel::StatusRole));
        QVERIFY(roles.contains(AccountListModel::ConnectionStateRole));
        QVERIFY(roles.contains(Qt::DisplayRole));
    }

    void reorderAndRemoveDoNotResetModels()
    {
        AccountListModel model;
        const QVector<AccountListModel::Entry> initial {
            {QStringLiteral("a"), QStringLiteral("A"), QString(), AccountConnectionState::Disconnected},
            {QStringLiteral("b"), QStringLiteral("B"), QString(), AccountConnectionState::Disconnected},
            {QStringLiteral("c"), QStringLiteral("C"), QString(), AccountConnectionState::Disconnected}
        };
        model.setEntries(initial, QStringLiteral("a"));
        QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        model.setEntries({initial[2], initial[0]}, QStringLiteral("c"));
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.idAt(0), QStringLiteral("c"));
        QCOMPARE(model.idAt(1), QStringLiteral("a"));
        QCOMPARE(moved.count(), 1);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(resets.count(), 0);
        QCOMPARE(model.selectedAccountId(), QStringLiteral("c"));
    }
};

QTEST_APPLESS_MAIN(AccountListModelTest)
#include "AccountListModelTest.moc"

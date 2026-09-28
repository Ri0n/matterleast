#include <QtTest>

#include "channel-tree-dialogs/UserGroupEditorPolicy.h"

using namespace Mattermost;

class UserGroupEditorPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void canonicalizesMention()
    {
        QCOMPARE(canonicalGroupMention(QStringLiteral("  @Example_Team  ")),
                 QStringLiteral("example_team"));
    }

    void suggestsMentionFromDisplayName()
    {
        QCOMPARE(suggestedGroupMention(QStringLiteral("Example Product Team")),
                 QStringLiteral("exampleproductteam"));
        QCOMPARE(suggestedGroupMention(QStringLiteral("QA / Release-Team")),
                 QStringLiteral("qareleaseteam"));
    }

    void rejectsReservedAndInvalidMentions()
    {
        QCOMPARE(validateUserGroupEditor(
                     QStringLiteral("Example Team"),
                     QStringLiteral("@channel"),
                     2,
                     true),
                 UserGroupValidationError::ReservedMention);

        QCOMPARE(validateUserGroupEditor(
                     QStringLiteral("Example Team"),
                     QStringLiteral("@bad mention"),
                     2,
                     true),
                 UserGroupValidationError::InvalidMention);
    }

    void creationRequiresAtLeastOneMember()
    {
        QCOMPARE(validateUserGroupEditor(
                     QStringLiteral("Example Team"),
                     QStringLiteral("@example-team"),
                     0,
                     true),
                 UserGroupValidationError::MissingMembers);

        QCOMPARE(validateUserGroupEditor(
                     QStringLiteral("Example Team"),
                     QStringLiteral("@example-team"),
                     1,
                     true),
                 UserGroupValidationError::None);
    }

    void existingGroupMayBecomeEmpty()
    {
        QCOMPARE(validateUserGroupEditor(
                     QStringLiteral("Example Team"),
                     QStringLiteral("@example-team"),
                     0,
                     false),
                 UserGroupValidationError::None);
    }
};

QTEST_APPLESS_MAIN(UserGroupEditorPolicyTest)
#include "UserGroupEditorPolicyTest.moc"

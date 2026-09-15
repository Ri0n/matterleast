#include <QtTest>

#include "backend/PostPageCursorMetadata.h"

using Mattermost::postPageCursorCreateAtById;

class PostPageCursorMetadataTest : public QObject
{
    Q_OBJECT
private slots:
    void cursorTimestampDoesNotRequireResidentBody()
    {
        constexpr std::uint64_t anchorCreateAt = 1757141150739ULL;
        QJsonObject posts;
        posts.insert(QStringLiteral("anchor-412"), QJsonObject {
            {QStringLiteral("id"), QStringLiteral("anchor-412")},
            {QStringLiteral("create_at"), QJsonValue(static_cast<double>(anchorCreateAt))},
        });

        const auto metadata = postPageCursorCreateAtById(
            posts, QStringList { QStringLiteral("anchor-412") });
        QCOMPARE(metadata.value(QStringLiteral("anchor-412")), anchorCreateAt);
    }
};

QTEST_APPLESS_MAIN(PostPageCursorMetadataTest)
#include "PostPageCursorMetadataTest.moc"

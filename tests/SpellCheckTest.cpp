/**
 * @file SpellCheckTest.cpp
 * @brief Tests for Unicode spelling and Markdown exclusions.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 *
 * MatterLeast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <QtTest>
#include <QTextBlock>
#include <QTextDocument>
#include "spellcheck/SpellChecker.h"
using namespace Mattermost;

class SpellCheckTest final : public QObject {
    Q_OBJECT
private slots:
    void wordsAreUnicodeAndKeepOffsets()
    {
        const QString line = QString::fromUtf8("Hello мир typo");
        const auto words = spellingWords(line);
        QCOMPARE(words.size(), 3);
        QCOMPARE(words.at(0).text, QStringLiteral("Hello"));
        QCOMPARE(words.at(0).start, 0);
        QCOMPARE(words.at(1).text, QString::fromUtf8("мир"));
        QCOMPARE(words.at(1).start, 6);
        QCOMPARE(words.at(2).start, 10);
        QCOMPARE(spellingWordAt(line, 8).text, QString::fromUtf8("мир"));
        QVERIFY(spellingWordAt(line, 9).text.isEmpty());
    }
    void ignoresMarkdownCodeUrlsAndMentions()
    {
        const QString line = QStringLiteral(
            "wrold `incorrect` https://example.org/incorrect @someone :custom_emoji: "
            "[label](https://example.org/wrong) finish");
        const auto words = spellingWords(line);
        QStringList found;
        for (const auto& word : words)
            found.append(word.text);
        QCOMPARE(found, QStringList({QStringLiteral("wrold"),
                                    QStringLiteral("label"),
                                    QStringLiteral("finish")}));
        QVERIFY(spellingWordAt(line, line.indexOf(QStringLiteral("incorrect"))).text.isEmpty());
    }
    void fencedCodeHasNoFormatting()
    {
        QTextDocument doc;
        SpellHighlighter highlighter(&doc);
        doc.setPlainText(QStringLiteral("before\n```cpp\nbroken\n```\nafter"));
        // QSyntaxHighlighter schedules block updates; force them before
        // asserting the fence state rather than relying on event-loop timing.
        highlighter.rehighlight();
        QTextBlock block = doc.begin();
        QCOMPARE(block.userState(), 0);
        block = block.next();
        QCOMPARE(block.userState(), 1);
        block = block.next();
        QCOMPARE(block.userState(), 1);
        block = block.next();
        QCOMPARE(block.userState(), 0);
        block = block.next();
        QCOMPARE(block.userState(), 0);
    }
};
QTEST_MAIN(SpellCheckTest)
#include "SpellCheckTest.moc"

/**
 * @file SpellChecker.h
 * @brief Shared spell-checking facade, tokenizer and highlighter.
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

#pragma once
#include <memory>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QSyntaxHighlighter>
namespace Mattermost {
struct SpellingWord {
    int start = 0;
    int length = 0;
    QString text;
};
QList<SpellingWord> spellingWords(const QString& line);
SpellingWord spellingWordAt(const QString& line, int position);
class SpellBackend {
public:
    virtual ~SpellBackend() = default;
    virtual QStringList languages() const = 0;
    virtual void configure(const QStringList& languages) = 0;
    virtual bool available() const = 0;
    virtual bool correct(const QString& word) const = 0;
    virtual QStringList suggestions(const QString& word) const = 0;
};
std::unique_ptr<SpellBackend> createPlatformSpellBackend();
class SpellChecker final : public QObject {
    Q_OBJECT
public:
    static SpellChecker& instance();
    bool available() const;
    bool correct(const QString& word) const;
    QStringList suggestions(const QString& word) const;
    QStringList languages() const;
    QStringList selectedLanguages() const;
    void setSelectedLanguages(const QStringList& languages);
    void ignore(const QString& word);
    void addToDictionary(const QString& word);
signals:
    void changed();
private:
    SpellChecker();
    void configureLanguages();
    std::unique_ptr<SpellBackend> _backend;
    QSet<QString> _sessionWords;
    QSet<QString> _customWords;
};
class SpellHighlighter final : public QSyntaxHighlighter {
public:
    explicit SpellHighlighter(QTextDocument* document);
protected:
    void highlightBlock(const QString& text) override;
};
} // namespace Mattermost

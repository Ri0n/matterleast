#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

namespace Mattermost {

class FontSettingsWidget final : public QWidget
{
    Q_OBJECT
public:
    struct Entry {
        QString id;
        QString label;
        QString font;
    };

    explicit FontSettingsWidget(const QVector<Entry>& fonts, QWidget* parent = nullptr);

signals:
    void fontChanged(const QString& id, const QString& serializedFont);
};

} // namespace Mattermost

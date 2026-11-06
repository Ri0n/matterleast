#include "FontSettingsWidget.h"

#include <memory>

#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontInfo>
#include <QGridLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QtMath>

namespace Mattermost {

FontSettingsWidget::FontSettingsWidget(const QVector<Entry>& fonts, QWidget* parent)
    : QWidget(parent)
{
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setColumnStretch(1, 1);
    int row = 0;
    for (const auto& entry : fonts) {
        auto state = std::make_shared<QFont>(font());
        QFont parsed;
        if (!entry.font.isEmpty() && parsed.fromString(entry.font)) *state = parsed;

        auto* label = new QLabel(entry.label, this);
        auto* family = new QFontComboBox(this);
        family->setObjectName(entry.id + QStringLiteral("FontFamily"));
        family->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        family->setMinimumContentsLength(14);
        family->setCurrentFont(*state);
        label->setBuddy(family);

        auto* size = new QDoubleSpinBox(this);
        size->setObjectName(entry.id + QStringLiteral("FontSize"));
        const qreal pointSize = state->pointSizeF() > 0
            ? state->pointSizeF() : QFontInfo(*state).pointSizeF();
        // Keep imported sizes representable even outside the usual slider range.
        size->setRange(qMin(8.0, pointSize > 0 ? pointSize : 8.0),
                       qMax(30.0, pointSize));
        size->setDecimals(1);
        size->setSuffix(tr(" pt"));
        size->setKeyboardTracking(false);
        size->setValue(pointSize > 0 ? pointSize : 10.0);
        size->setAccessibleName(tr("%1 font size").arg(entry.label));

        auto* slider = new QSlider(Qt::Horizontal, this);
        slider->setObjectName(entry.id + QStringLiteral("FontSizeSlider"));
        slider->setRange(qFloor(size->minimum()), qCeil(size->maximum()));
        slider->setPageStep(2);
        slider->setMinimumWidth(60);
        slider->setMaximumWidth(120);
        slider->setValue(qRound(size->value()));
        slider->setAccessibleName(size->accessibleName());

        grid->addWidget(label, row, 0);
        grid->addWidget(family, row, 1);
        grid->addWidget(size, row, 2);
        grid->addWidget(slider, row, 3);
        ++row;

        connect(family, &QFontComboBox::currentFontChanged, this,
                [this, state, id = entry.id](const QFont& selected) {
            state->setFamily(selected.family());
            emit fontChanged(id, state->toString());
        });
        connect(size, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, state, slider, id = entry.id](double points) {
            const QSignalBlocker blocker(slider);
            slider->setValue(qRound(points));
            state->setPointSizeF(points);
            emit fontChanged(id, state->toString());
        });
        connect(slider, &QSlider::valueChanged, size, &QDoubleSpinBox::setValue);
    }
}

} // namespace Mattermost

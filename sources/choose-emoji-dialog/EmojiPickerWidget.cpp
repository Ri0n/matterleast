#include "EmojiPickerWidget.h"

namespace Mattermost {

EmojiPickerWidget::EmojiPickerWidget(Backend& backend, QWidget* parent)
    : QWidget(parent)
    , backend_(backend)
{
}

void EmojiPickerWidget::prepare() {}
void EmojiPickerWidget::resetSearch() {}
void EmojiPickerWidget::focusSearch() {}

} // namespace Mattermost

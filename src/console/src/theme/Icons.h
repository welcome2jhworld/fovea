#pragma once
#include <QColor>
#include <QIcon>

namespace fovea::ui {

enum class Icon { CaretDown, CaretRight, Square, Gear, Close, Maximize };

// Vector glyphs from resources/icons tinted with a token colour. Plex has no
// coverage for these code points, so they are never drawn as text.
QIcon themedIcon(Icon icon, const QColor& color);

}

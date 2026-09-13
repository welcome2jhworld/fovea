#pragma once
#include "theme/Tone.h"
#include <QColor>
#include <QFont>
#include <QPainter>
#include <QRect>
#include <QRectF>
#include <QString>

namespace fovea::ui {

int chipWidth(const QFont& font, const QString& text, int paddingX);
// Tone text on its .14 tint (severity, status and evidence chips).
QRect paintToneChip(QPainter& p, const QPoint& topLeft, const QString& text, Tone tone, int height, int paddingX,
                    const QFont& font);
// Raised fill with a 1px border (camera id and scope chips).
// 115° placeholder stripes (#141618 / #0E0F11) where no frame or thumbnail exists.
void paintPlaceholderStripes(QPainter& p, const QRectF& rect, double opacity = 1.0);
QRect paintOutlineChip(QPainter& p, const QPoint& topLeft, const QString& text, int height, int paddingX,
                       const QFont& font, const QColor& border, const QColor& textColor);

}

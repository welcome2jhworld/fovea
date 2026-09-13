#include "widgets/Painting.h"
#include "theme/Tokens.h"
#include <QFontMetrics>
#include <QLinearGradient>
#include <QtMath>
#include <cmath>

namespace fovea::ui {
namespace tk = tokens;

int chipWidth(const QFont& font, const QString& text, int paddingX) {
  return 2 * paddingX + QFontMetrics(font).horizontalAdvance(text);
}

QRect paintToneChip(QPainter& p, const QPoint& topLeft, const QString& text, Tone tone, int height, int paddingX,
                    const QFont& font) {
  const QRect rect(topLeft, QSize(chipWidth(font, text, paddingX), height));
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(Qt::NoPen);
  p.setBrush(toneTint(tone));
  p.drawRoundedRect(rect, tk::radius::chip, tk::radius::chip);
  p.setFont(font);
  p.setPen(toneColor(tone));
  p.drawText(rect, Qt::AlignCenter, text);
  p.restore();
  return rect;
}

void paintPlaceholderStripes(QPainter& p, const QRectF& rect, double opacity) {
  const double rad = qDegreesToRadians(tk::stripe::angleDeg);
  const QPointF dir(std::sin(rad), -std::cos(rad));
  QLinearGradient gradient(QPointF(0, 0), dir * tk::stripe::period);
  gradient.setSpread(QGradient::RepeatSpread);
  const double split = static_cast<double>(tk::stripe::lightBand) / tk::stripe::period;
  const QColor light = tk::color::q(tk::color::stripeLight);
  const QColor dark = tk::color::q(tk::color::stripeDark);
  gradient.setColorAt(0.0, light);
  gradient.setColorAt(split, light);
  gradient.setColorAt(split + 0.0001, dark);
  gradient.setColorAt(1.0, dark);
  p.save();
  p.setOpacity(opacity);
  p.fillRect(rect, gradient);
  p.restore();
}

QRect paintOutlineChip(QPainter& p, const QPoint& topLeft, const QString& text, int height, int paddingX,
                       const QFont& font, const QColor& border, const QColor& textColor) {
  const QRect rect(topLeft, QSize(chipWidth(font, text, paddingX), height));
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(QPen(border, 1.0));
  p.setBrush(tk::color::q(tk::color::bgRaised));
  p.drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), tk::radius::chip, tk::radius::chip);
  p.setFont(font);
  p.setPen(textColor);
  p.drawText(rect, Qt::AlignCenter, text);
  p.restore();
  return rect;
}

}

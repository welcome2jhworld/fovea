#include "theme/Icons.h"
#include <QFile>
#include <QIconEngine>
#include <QPainter>
#include <QSvgRenderer>
#include <algorithm>

namespace fovea::ui {

namespace {
const char* iconPath(Icon icon) {
  switch (icon) {
    case Icon::CaretDown: return ":/icons/caret-down.svg";
    case Icon::CaretRight: return ":/icons/caret-right.svg";
    case Icon::Square: return ":/icons/square.svg";
    case Icon::Gear: return ":/icons/gear.svg";
    case Icon::Close: return ":/icons/close.svg";
    case Icon::Maximize: return ":/icons/maximize.svg";
  }
  return "";
}

class SvgIconEngine : public QIconEngine {
public:
  explicit SvgIconEngine(QByteArray svg) : svg_(std::move(svg)), renderer_(svg_) {}

  void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override {
    const int side = std::min(rect.width(), rect.height());
    const QRect square(rect.left() + (rect.width() - side) / 2, rect.top() + (rect.height() - side) / 2, side, side);
    painter->setRenderHint(QPainter::Antialiasing, true);
    renderer_.render(painter, square);
  }

  QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    paint(&p, QRect(QPoint(0, 0), size), mode, state);
    return pm;
  }

  QIconEngine* clone() const override { return new SvgIconEngine(svg_); }

private:
  QByteArray svg_;
  QSvgRenderer renderer_;
};
}

QIcon themedIcon(Icon icon, const QColor& color) {
  QFile file(QLatin1StringView(iconPath(icon)));
  if (!file.open(QIODevice::ReadOnly)) return {};
  QByteArray svg = file.readAll();
  svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
  return QIcon(new SvgIconEngine(std::move(svg)));
}

}

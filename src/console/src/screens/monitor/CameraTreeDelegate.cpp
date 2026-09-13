#include "screens/monitor/CameraTreeDelegate.h"
#include "screens/monitor/CameraTreeModel.h"
#include "theme/Icons.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QFontMetrics>
#include <QPainter>

namespace fovea::ui {
namespace tk = tokens;

CameraTreeDelegate::CameraTreeDelegate(QObject* parent)
    : QStyledItemDelegate(parent),
      groupOpen_(themedIcon(Icon::CaretDown, tk::color::q(tk::color::textDisabled))),
      groupClosed_(themedIcon(Icon::CaretRight, tk::color::q(tk::color::textDisabled))),
      camera_(themedIcon(Icon::Square, tk::color::q(tk::color::textDisabled))) {}

QSize CameraTreeDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
  return QSize(option.rect.width(), tk::size::treeRow + tk::size::treeRowGap);
}

void CameraTreeDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  const bool isGroup = index.data(CameraTreeModel::IsGroupRole).toBool();
  const bool selected = option.state & QStyle::State_Selected;
  const QRect row(option.rect.left(), option.rect.top(), option.rect.width(), tk::size::treeRow);
  if (selected && !isGroup) {
    painter->setPen(Qt::NoPen);
    painter->setBrush(tk::color::q(tk::color::bgRaised));
    painter->drawRoundedRect(row, tk::radius::control, tk::radius::control);
  }

  const QIcon& glyph = isGroup ? ((option.state & QStyle::State_Open) ? groupOpen_ : groupClosed_) : camera_;
  int x = row.left() + tk::size::treeRowPaddingX;
  glyph.paint(painter, QRect(x, row.top() + (row.height() - tk::size::glyphTree) / 2, tk::size::glyphTree, tk::size::glyphTree));
  x += tk::size::glyphTree + tk::size::treeGlyphGap;

  const int dotSpace = isGroup ? 0 : tk::size::treeDot + tk::size::treeGlyphGap;
  const int textRight = row.right() - tk::size::treeRowPaddingX - dotSpace;
  painter->setFont(Theme::body());
  painter->setPen(tk::color::q(isGroup ? tk::color::textPrimary : tk::color::textSecondary));
  const QFontMetrics textMetrics(painter->font());
  const QString text = textMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRight - x);
  painter->drawText(QRect(x, row.top(), textRight - x, row.height()), Qt::AlignVCenter | Qt::AlignLeft, text);

  if (!isGroup) {
    const bool online = index.data(CameraTreeModel::OnlineRole).toBool();
    painter->setPen(Qt::NoPen);
    painter->setBrush(tk::color::q(online ? tk::color::positive : tk::color::textMuted));
    const int dotX = row.right() - tk::size::treeRowPaddingX - tk::size::treeDot + 1;
    const int dotY = row.top() + (row.height() - tk::size::treeDot) / 2;
    painter->drawEllipse(QRect(dotX, dotY, tk::size::treeDot, tk::size::treeDot));
  }
  painter->restore();
}

}

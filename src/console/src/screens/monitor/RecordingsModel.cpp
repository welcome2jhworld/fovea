#include "screens/monitor/RecordingsModel.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "util/Format.h"
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kPaddingX = 16;
constexpr int kPaddingTop = 10;
constexpr int kLineGap = 6;
constexpr int kChipHeight = 19;
constexpr int kChipPaddingX = 6;
constexpr int kPlayWidth = 44;

struct ChipStyle {
  QColor color;
  QColor tint;
};

ChipStyle chipStyle(const QString& state) {
  if (state == QLatin1StringView("recording")) return {tk::color::q(tk::color::accent), tk::color::tintAccent()};
  if (state == QLatin1StringView("finalized")) return {tk::color::q(tk::color::positive), tk::color::tintPositive()};
  if (state == QLatin1StringView("damaged")) return {tk::color::q(tk::color::critical), tk::color::tintCritical()};
  if (state == QLatin1StringView("gap")) return {tk::color::q(tk::color::warning), tk::color::tintWarning()};
  return {tk::color::q(tk::color::textMuted), tk::color::tintNeutral()};
}

int drawChip(QPainter* p, int x, int y, const QString& text, const ChipStyle& style) {
  const QFont font = Theme::monoMicro();
  const QFontMetrics fm(font);
  const QRect chip(x, y, fm.horizontalAdvance(text) + 2 * kChipPaddingX, kChipHeight);
  p->setPen(Qt::NoPen);
  p->setBrush(style.tint);
  p->drawRoundedRect(chip, tk::radius::chip, tk::radius::chip);
  p->setFont(font);
  p->setPen(style.color);
  p->drawText(chip, Qt::AlignCenter, text);
  return chip.right() + 1;
}
}

RecordingsModel::RecordingsModel(QObject* parent) : QAbstractListModel(parent) {}

void RecordingsModel::setRows(QVector<fovea::RecordingSegment> segments, QVector<fovea::ReceiveGap> gaps, int64_t nowUtcMs) {
  QVector<RecordingRow> rows;
  rows.reserve(segments.size() + gaps.size());
  for (fovea::RecordingSegment& s : segments) {
    RecordingRow r;
    r.kind = RecordingRow::Kind::Segment;
    r.sortUtcMs = s.startUtcMs;
    r.segment = std::move(s);
    rows.push_back(std::move(r));
  }
  for (fovea::ReceiveGap& g : gaps) {
    RecordingRow r;
    r.kind = RecordingRow::Kind::Gap;
    r.sortUtcMs = g.fromUtcMs;
    r.gap = std::move(g);
    rows.push_back(std::move(r));
  }
  std::stable_sort(rows.begin(), rows.end(), [](const RecordingRow& a, const RecordingRow& b) { return a.sortUtcMs > b.sortUtcMs; });
  beginResetModel();
  rows_ = std::move(rows);
  nowUtcMs_ = nowUtcMs;
  endResetModel();
}

int RecordingsModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant RecordingsModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid()) return {};
  const RecordingRow& r = rows_[index.row()];
  switch (role) {
    case KindRole: return static_cast<int>(r.kind);
    case PlayableRole: return r.kind == RecordingRow::Kind::Segment && r.segment.state == QLatin1StringView("finalized");
    case Qt::ToolTipRole:
      return r.kind == RecordingRow::Kind::Segment ? r.segment.path : r.gap.reason;
    default: return {};
  }
}

RecordingRowDelegate::RecordingRowDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

QSize RecordingRowDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
  return QSize(option.rect.width(), kRowHeight);
}

QRect RecordingRowDelegate::playRect(const QRect& row) {
  return QRect(row.right() - kPaddingX - kPlayWidth + 1, row.bottom() - kPaddingTop - kChipHeight, kPlayWidth, kChipHeight);
}

void RecordingRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
  const auto* model = static_cast<const RecordingsModel*>(index.model());
  const RecordingRow& r = model->rowAt(index);
  const QRect row = option.rect;
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);

  const QFont titleFont = Theme::body();
  const QFont monoFont = Theme::mono(tk::font::label);
  const QFontMetrics titleMetrics(titleFont);
  const int textLeft = row.left() + kPaddingX;
  const int textRight = row.right() - kPaddingX;
  const QRect line1(textLeft, row.top() + kPaddingTop, textRight - textLeft, titleMetrics.height());
  const int line2Top = line1.bottom() + 1 + kLineGap;

  QString title;
  QString right;
  QString chipText;
  QString detail;
  if (r.kind == RecordingRow::Kind::Segment) {
    const fovea::RecordingSegment& s = r.segment;
    title = localTimeLabel(s.startUtcMs);
    const bool open = s.state == QLatin1StringView("recording") || s.endUtcMs <= s.startUtcMs;
    const int64_t lengthMs = open ? model->nowUtcMs() - s.startUtcMs : s.endUtcMs - s.startUtcMs;
    right = durationLabel(lengthMs);
    chipText = s.state.toUpper();
    painter->setPen(tk::color::q(tk::color::textPrimary));
  } else {
    const fovea::ReceiveGap& g = r.gap;
    const bool open = g.toUtcMs <= g.fromUtcMs;
    title = QStringLiteral("%1 → %2").arg(localTimeLabel(g.fromUtcMs), open ? QStringLiteral("now") : localTimeLabel(g.toUtcMs));
    right = durationLabel((open ? model->nowUtcMs() : g.toUtcMs) - g.fromUtcMs);
    chipText = QStringLiteral("GAP");
    detail = g.reason;
    painter->setPen(tk::color::q(tk::color::textSecondary));
  }

  painter->setFont(titleFont);
  const int rightWidth = QFontMetrics(monoFont).horizontalAdvance(right) + 12;
  painter->drawText(line1, Qt::AlignVCenter | Qt::AlignLeft,
                    titleMetrics.elidedText(title, Qt::ElideRight, line1.width() - rightWidth));
  painter->setFont(monoFont);
  painter->setPen(tk::color::q(tk::color::textMuted));
  painter->drawText(line1, Qt::AlignVCenter | Qt::AlignRight, right);

  const ChipStyle style = chipStyle(r.kind == RecordingRow::Kind::Gap ? QStringLiteral("gap") : r.segment.state);
  int x = drawChip(painter, textLeft, line2Top, chipText, style);
  if (!detail.isEmpty()) {
    painter->setFont(monoFont);
    painter->setPen(tk::color::q(tk::color::textMuted));
    const QRect detailRect(x + 8, line2Top, textRight - x - 8, kChipHeight);
    painter->drawText(detailRect, Qt::AlignVCenter | Qt::AlignLeft,
                      QFontMetrics(monoFont).elidedText(detail, Qt::ElideRight, detailRect.width()));
  }
  if (index.data(RecordingsModel::PlayableRole).toBool()) {
    painter->setFont(Theme::small());
    painter->setPen(tk::color::q(tk::color::accent));
    painter->drawText(playRect(row), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("▶ Play"));
  }

  painter->setPen(Qt::NoPen);
  painter->setBrush(tk::color::q(tk::color::lineRow));
  painter->drawRect(QRect(row.left(), row.bottom(), row.width(), 1));
  painter->restore();
}

bool RecordingRowDelegate::editorEvent(QEvent* event, QAbstractItemModel*, const QStyleOptionViewItem& option,
                                       const QModelIndex& index) {
  if (event->type() != QEvent::MouseButtonRelease) return false;
  auto* mouse = static_cast<QMouseEvent*>(event);
  if (mouse->button() != Qt::LeftButton || !index.data(RecordingsModel::PlayableRole).toBool()) return false;
  if (!playRect(option.rect).adjusted(-6, -6, 6, 6).contains(mouse->position().toPoint())) return false;
  emit playClicked(index);
  return true;
}

}

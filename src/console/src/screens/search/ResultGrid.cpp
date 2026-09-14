#include "screens/search/ResultGrid.h"
#include "core/ThumbnailCache.h"
#include "search/SearchLogic.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "util/Format.h"
#include "widgets/Painting.h"
#include <QKeyEvent>
#include <QScrollBar>
#include <QStyle>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kBodyPaddingTop = 10;
constexpr int kBodyPaddingX = 12;
constexpr int kBodyPaddingBottom = 12;
constexpr int kBodyGap = 7;
constexpr int kBadgeInset = 8;
constexpr int kBadgeHeight = 20;
constexpr int kBadgePaddingX = 7;
constexpr int kSkeletonBarHeight = 10;

QFont titleFont() { return Theme::sans(tk::font::body, QFont::Medium); }
QFont metaFont() { return Theme::mono(tk::font::label); }

// Letterboxed on the video fill: the representative frame is shown whole, never cropped.
void drawContained(QPainter& p, const QRectF& target, const QImage& image) {
  const double scale = std::min(target.width() / image.width(), target.height() / image.height());
  const QSizeF size(image.width() * scale, image.height() * scale);
  const QRectF at(QPointF(target.center().x() - size.width() / 2.0, target.center().y() - size.height() / 2.0), size);
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  p.drawImage(at, image);
}

void paintBadge(QPainter& p, const QRect& rect, const QString& text, const QColor& fill, const QColor& ink) {
  p.setPen(Qt::NoPen);
  p.setBrush(fill);
  p.drawRoundedRect(rect, tk::radius::chip, tk::radius::chip);
  p.setFont(Theme::monoMicro());
  p.setPen(ink);
  p.drawText(rect, Qt::AlignCenter, text);
}
}

SearchResultsModel::SearchResultsModel(QObject* parent) : QAbstractListModel(parent) {}

void SearchResultsModel::setRows(QVector<SearchResultRow> rows) {
  beginResetModel();
  rows_ = std::move(rows);
  skeleton_ = 0;
  endResetModel();
}

void SearchResultsModel::setSkeleton(int count) {
  beginResetModel();
  rows_.clear();
  skeleton_ = count;
  endResetModel();
}

int SearchResultsModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return skeleton_ > 0 ? skeleton_ : static_cast<int>(rows_.size());
}

QVariant SearchResultsModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || isSkeleton() || index.row() >= rows_.size()) return {};
  const SearchResultRow& row = rows_[index.row()];
  if (role == Qt::DisplayRole) return row.cameraName;
  if (role == Qt::ToolTipRole) return relevanceLabel(row.result.relevance);
  return {};
}

Qt::ItemFlags SearchResultsModel::flags(const QModelIndex& index) const {
  if (!index.isValid() || isSkeleton()) return Qt::NoItemFlags;
  return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

ResultCardDelegate::ResultCardDelegate(ThumbnailCache& thumbnails, QObject* parent)
    : QStyledItemDelegate(parent), thumbnails_(thumbnails) {}

int ResultCardDelegate::cardHeight() {
  return 1 + kThumbHeight + kBodyPaddingTop + QFontMetrics(titleFont()).height() + kBodyGap +
         QFontMetrics(metaFont()).height() + kBodyPaddingBottom + 1;
}

// The cell is the view's grid size, so each card fills its cell less the gap.
QSize ResultCardDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
  const auto* view = qobject_cast<const QListView*>(option.widget);
  if (view && view->gridSize().isValid()) return view->gridSize();
  return QSize(ResultGrid::kMinCardWidth + kGap, cardHeight() + kGap);
}

void ResultCardDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
  const auto* model = static_cast<const SearchResultsModel*>(index.model());
  const bool skeleton = model->isSkeleton();
  const bool selected = !skeleton && (option.state & QStyle::State_Selected);
  QPainter& p = *painter;
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);

  const QRectF card = QRectF(option.rect.adjusted(0, 0, -kGap, -kGap)).adjusted(0.5, 0.5, -0.5, -0.5);
  QPainterPath outline;
  outline.addRoundedRect(card, tk::radius::card, tk::radius::card);
  p.fillPath(outline, tk::color::q(tk::color::bgPanel));

  const QRectF thumb(card.left() + 0.5, card.top() + 0.5, card.width() - 1.0, kThumbHeight);
  p.save();
  p.setClipPath(outline);
  p.fillRect(thumb, tk::color::q(tk::color::bgVideo));
  const SearchResultRow* row = skeleton ? nullptr : &model->rowAt(index.row());
  const QImage image = row ? thumbnails_.thumbnail(row->result.recordId) : QImage();
  if (image.isNull()) paintPlaceholderStripes(p, thumb);
  else drawContained(p, thumb, image);
  p.restore();

  const QFont title = titleFont();
  const QFont meta = metaFont();
  const QFontMetrics titleMetrics(title);
  const QFontMetrics metaMetrics(meta);
  const int bodyLeft = static_cast<int>(card.left()) + kBodyPaddingX;
  const int bodyWidth = static_cast<int>(card.width()) - 2 * kBodyPaddingX;
  const int titleTop = static_cast<int>(thumb.bottom()) + kBodyPaddingTop;
  const int metaTop = titleTop + titleMetrics.height() + kBodyGap;

  if (row) {
    const QString relevance = relevanceLabel(row->result.relevance);
    const QRect relevanceRect(static_cast<int>(thumb.left()) + kBadgeInset, static_cast<int>(thumb.top()) + kBadgeInset,
                              chipWidth(Theme::monoMicro(), relevance, kBadgePaddingX), kBadgeHeight);
    paintBadge(p, relevanceRect, relevance, tk::color::accentBadge(), tk::color::q(tk::color::accentInk));
    const QString length = resultLengthLabel(row->result);
    const int lengthWidth = chipWidth(Theme::monoMicro(), length, kBadgePaddingX);
    const QRect lengthRect(static_cast<int>(thumb.right()) - kBadgeInset - lengthWidth,
                           static_cast<int>(thumb.bottom()) - kBadgeInset - kBadgeHeight, lengthWidth, kBadgeHeight);
    paintBadge(p, lengthRect, length, tk::color::scrimBadge(), tk::color::q(tk::color::textSecondary));

    p.setFont(title);
    p.setPen(tk::color::q(tk::color::textPrimary));
    p.drawText(QRect(bodyLeft, titleTop, bodyWidth, titleMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
               titleMetrics.elidedText(row->cameraName, Qt::ElideRight, bodyWidth));
    const QString time = localTimeLabel(row->result.startUtcMs);
    const int timeWidth = metaMetrics.horizontalAdvance(time);
    p.setFont(meta);
    p.setPen(tk::color::q(tk::color::textMuted));
    const QRect metaRow(bodyLeft, metaTop, bodyWidth, metaMetrics.height());
    p.drawText(metaRow, Qt::AlignRight | Qt::AlignVCenter, time);
    p.drawText(metaRow.adjusted(0, 0, -(timeWidth + kBodyPaddingX), 0), Qt::AlignLeft | Qt::AlignVCenter,
               metaMetrics.elidedText(row->cameraCode, Qt::ElideRight, std::max(0, bodyWidth - timeWidth - kBodyPaddingX)));
  } else {
    p.setPen(Qt::NoPen);
    p.setBrush(tk::color::q(tk::color::bgRaised));
    const int titleBarTop = titleTop + (titleMetrics.height() - kSkeletonBarHeight) / 2;
    const int metaBarTop = metaTop + (metaMetrics.height() - kSkeletonBarHeight) / 2;
    const int timeBar = bodyWidth * 18 / 100;
    for (const QRect& bar : {QRect(bodyLeft, titleBarTop, bodyWidth * 45 / 100, kSkeletonBarHeight),
                             QRect(bodyLeft, metaBarTop, bodyWidth * 22 / 100, kSkeletonBarHeight),
                             QRect(bodyLeft + bodyWidth - timeBar, metaBarTop, timeBar, kSkeletonBarHeight)})
      p.drawRoundedRect(bar, tk::radius::box, tk::radius::box);
  }

  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(tk::color::q(selected ? tk::color::accent : tk::color::line), 1.0));
  p.drawPath(outline);
  p.restore();
}

ResultGrid::ResultGrid(QWidget* parent) : QListView(parent) {
  setObjectName(QStringLiteral("ResultGrid"));
  setViewMode(QListView::IconMode);
  setFlow(QListView::LeftToRight);
  setWrapping(true);
  setMovement(QListView::Static);
  setResizeMode(QListView::Adjust);
  setUniformItemSizes(true);
  setSpacing(0);
  setDragEnabled(false);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setFrameShape(QFrame::NoFrame);
  setMouseTracking(false);
  viewport()->setAutoFillBackground(false);
}

void ResultGrid::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Space && (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
    emit playbackToggleRequested();
    event->accept();
    return;
  }
  QListView::keyPressEvent(event);
}

void ResultGrid::resizeEvent(QResizeEvent* event) {
  updateGrid();
  QListView::resizeEvent(event);
}

void ResultGrid::updateGeometries() {
  QListView::updateGeometries();
  updateGrid();
}

// Cells include the gap on their right and bottom edges. QListView lays a left-to-right flow
// out in the viewport less the scroll bar extent, whether or not the bar shows, and wraps an
// item that would reach the last pixel, so the cell width keeps that margin.
void ResultGrid::updateGrid() {
  const int available = maximumViewportSize().width() -
                        style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, verticalScrollBar()) - kWrapSlack;
  const int stride = ResultCardDelegate::kGap;
  const int columns = std::clamp((available + stride) / (kMinCardWidth + stride), 1, kColumns);
  const QSize grid(std::max(1, available / columns), ResultCardDelegate::cardHeight() + stride);
  if (grid != gridSize()) setGridSize(grid);
}

}

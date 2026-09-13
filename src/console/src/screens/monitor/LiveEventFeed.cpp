#include "screens/monitor/LiveEventFeed.h"
#include "alerts/AlertLogic.h"
#include "core/EventStore.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "util/Format.h"
#include "widgets/Painting.h"
#include <QActionGroup>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kRowPaddingX = 16;
constexpr int kRowPaddingY = 12;
constexpr int kBarWidth = 3;
constexpr int kBarGap = 10;
constexpr int kTitleHeight = 17;
constexpr int kLineGap = 5;
constexpr int kDetailHeight = 16;
constexpr int kChipHeight = 19;
constexpr int kChipPaddingX = 6;
constexpr int kChipGap = 6;
constexpr int kTimeGap = 8;
constexpr int kFooterPaddingY = 12;

QString stateChipText(const EventInfo& e) {
  if (e.review && e.review->label == QLatin1StringView("false_alarm")) return QStringLiteral("FALSE ALARM");
  if (e.operatorState == QLatin1StringView("resolved")) return QStringLiteral("RESOLVED");
  if (e.operatorState == QLatin1StringView("acknowledged")) return QStringLiteral("ACKNOWLEDGED");
  return {};
}
}

EventFeedModel::EventFeedModel(QObject* parent) : QAbstractListModel(parent) {}

void EventFeedModel::setRows(QVector<FeedRow> rows) {
  beginResetModel();
  rows_ = std::move(rows);
  endResetModel();
}

int EventFeedModel::indexOf(const QString& eventId) const {
  for (int i = 0; i < rows_.size(); ++i)
    if (rows_[i].event.id == eventId) return i;
  return -1;
}

int EventFeedModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant EventFeedModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= rows_.size()) return {};
  if (role == Qt::DisplayRole) return rows_[index.row()].event.title;
  return {};
}

EventRowDelegate::EventRowDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

QSize EventRowDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
  return QSize(option.rect.width(), kRowHeight);
}

void EventRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
  const auto* model = static_cast<const EventFeedModel*>(index.model());
  const FeedRow& row = model->rowAt(index.row());
  const EventInfo& e = row.event;
  QPainter& p = *painter;
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  const QRect r = option.rect;
  const Tone tone = severityTone(e.severity);
  const bool selected = option.state & QStyle::State_Selected;
  if (selected) p.fillRect(r, tk::color::q(tk::color::bgPanel));
  if (index.row() == 0 && tone == Tone::Critical && alertBucket(e) == AlertBucket::Unresolved)
    p.fillRect(r, tk::color::tintCriticalRow());
  p.fillRect(QRect(r.left(), r.bottom(), r.width(), 1), tk::color::q(tk::color::lineRow));

  const QRect content = r.adjusted(kRowPaddingX, kRowPaddingY, -kRowPaddingX, -kRowPaddingY - 1);
  p.setPen(Qt::NoPen);
  p.setBrush(toneColor(tone));
  p.drawRoundedRect(QRect(content.left(), content.top(), kBarWidth, content.height()), tk::radius::box, tk::radius::box);

  const int x = content.left() + kBarWidth + kBarGap;
  const int w = content.right() + 1 - x;
  const QFont titleFont = Theme::sans(tk::font::body, QFont::Medium);
  const QFont timeFont = Theme::mono(tk::font::label);
  const QFontMetrics titleMetrics(titleFont);
  const QFontMetrics timeMetrics(timeFont);
  const QString time = localTimeLabel(e.openedUtcMs);
  const int timeW = timeMetrics.horizontalAdvance(time);
  const int baseline = content.top() + titleMetrics.ascent() + (kTitleHeight - titleMetrics.height()) / 2;
  p.setFont(titleFont);
  p.setPen(tk::color::q(tk::color::textPrimary));
  p.drawText(QPoint(x, baseline), titleMetrics.elidedText(e.title, Qt::ElideRight, w - timeW - kTimeGap));
  p.setFont(timeFont);
  p.setPen(tk::color::q(tk::color::textMuted));
  p.drawText(QPoint(x + w - timeW, baseline), time);

  const QFont detailFont = Theme::small();
  const QFontMetrics detailMetrics(detailFont);
  const QRect detail(x, content.top() + kTitleHeight + kLineGap, w, kDetailHeight);
  p.setFont(detailFont);
  p.setPen(tk::color::q(tk::color::textSecondary));
  p.drawText(detail, Qt::AlignVCenter | Qt::AlignLeft, detailMetrics.elidedText(e.detail, Qt::ElideRight, w));

  const QFont chipFont = Theme::monoMicro();
  QPoint chip(x, detail.bottom() + 1 + kLineGap);
  if (!row.cameraLabel.isEmpty()) {
    const QRect cam = paintOutlineChip(p, chip, row.cameraLabel, kChipHeight, kChipPaddingX, chipFont,
                                       tk::color::q(tk::color::line), tk::color::q(tk::color::textSecondary));
    chip.setX(cam.right() + 1 + kChipGap);
  }
  const QRect sev = paintToneChip(p, chip, severityLabel(e.severity), tone, kChipHeight, kChipPaddingX, chipFont);
  const QString state = stateChipText(e);
  if (!state.isEmpty()) {
    const Tone stateTone = e.operatorState == QLatin1StringView("resolved") && state != QLatin1StringView("FALSE ALARM")
                               ? Tone::Positive
                               : Tone::Neutral;
    paintToneChip(p, QPoint(sev.right() + 1 + kChipGap, chip.y()), state, stateTone, kChipHeight, kChipPaddingX, chipFont);
  }
  p.restore();
}

LiveEventFeed::LiveEventFeed(EventStore& store, QWidget* parent) : QWidget(parent), store_(store) {
  setObjectName(QStringLiteral("LiveEventFeed"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedWidth(tk::size::inspector);

  auto* header = new QWidget(this);
  header->setObjectName(QStringLiteral("FeedHeader"));
  header->setAttribute(Qt::WA_StyledBackground, true);
  header->setFixedHeight(tk::size::panelHeader);
  auto* title = new QLabel(QStringLiteral("LIVE EVENTS"), header);
  title->setProperty("role", QStringLiteral("section-label"));
  title->setFont(Theme::monoLabel());
  auto* filter = new QPushButton(QStringLiteral("Filter"), header);
  filter->setProperty("role", QStringLiteral("link"));
  filter->setCursor(Qt::PointingHandCursor);
  filter->setFocusPolicy(Qt::NoFocus);
  auto* filterMenu = new QMenu(filter);
  auto* group = new QActionGroup(filterMenu);
  QAction* all = filterMenu->addAction(QStringLiteral("All events"));
  QAction* unresolved = filterMenu->addAction(QStringLiteral("Unresolved only"));
  for (QAction* a : {all, unresolved}) {
    a->setCheckable(true);
    group->addAction(a);
  }
  all->setChecked(true);
  auto* headerRow = new QHBoxLayout(header);
  headerRow->setContentsMargins(tk::size::railHeaderPaddingX, 0, tk::size::railHeaderPaddingX, 0);
  headerRow->addWidget(title);
  headerRow->addStretch(1);
  headerRow->addWidget(filter);

  model_ = new EventFeedModel(this);
  list_ = new QListView(this);
  list_->setObjectName(QStringLiteral("EventList"));
  list_->setModel(model_);
  list_->setItemDelegate(new EventRowDelegate(list_));
  list_->setFrameShape(QFrame::NoFrame);
  list_->setSelectionMode(QAbstractItemView::SingleSelection);
  list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_->setFocusPolicy(Qt::NoFocus);
  list_->setUniformItemSizes(true);
  list_->viewport()->setAutoFillBackground(false);

  empty_ = new QLabel(this);
  empty_->setObjectName(QStringLiteral("FeedEmpty"));
  empty_->setAlignment(Qt::AlignCenter);
  empty_->setWordWrap(true);
  empty_->setContentsMargins(16, 20, 16, 20);

  auto* footer = new QWidget(this);
  footer->setObjectName(QStringLiteral("FeedFooter"));
  footer->setAttribute(Qt::WA_StyledBackground, true);
  acknowledge_ = new QPushButton(QStringLiteral("Acknowledge"), footer);
  acknowledge_->setProperty("role", QStringLiteral("secondary"));
  openCase_ = new QPushButton(QStringLiteral("Open case"), footer);
  openCase_->setProperty("role", QStringLiteral("primary"));
  for (QPushButton* b : {acknowledge_, openCase_}) {
    Theme::setVariant(b, "lg");
    b->setCursor(Qt::PointingHandCursor);
  }
  auto* footerRow = new QHBoxLayout(footer);
  footerRow->setContentsMargins(kRowPaddingX, kFooterPaddingY, kRowPaddingX, kFooterPaddingY);
  footerRow->setSpacing(8);
  footerRow->addWidget(acknowledge_, 1);
  footerRow->addWidget(openCase_, 1);

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(header);
  column->addWidget(empty_);
  column->addWidget(list_, 1);
  column->addWidget(footer);

  connect(filter, &QPushButton::clicked, this, [filter, filterMenu] {
    filterMenu->popup(filter->mapToGlobal(QPoint(filter->width() - filterMenu->sizeHint().width(), filter->height() + 6)));
  });
  connect(unresolved, &QAction::toggled, this, [this](bool on) {
    unresolvedOnly_ = on;
    rebuild();
  });
  connect(list_, &QListView::clicked, this, [this](const QModelIndex& index) {
    if (!index.isValid()) return;
    followNewest_ = false;
    select(model_->rowAt(index.row()).event.id);
  });
  connect(list_, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
    if (index.isValid()) emit openCaseRequested(model_->rowAt(index.row()).event.id);
  });
  connect(acknowledge_, &QPushButton::clicked, this, [this] {
    if (!selectedId_.isEmpty()) store_.acknowledge(selectedId_);
  });
  connect(openCase_, &QPushButton::clicked, this, [this] {
    if (!selectedId_.isEmpty()) emit openCaseRequested(selectedId_);
  });
  connect(&store_, &EventStore::eventsChanged, this, &LiveEventFeed::rebuild);
  rebuild();
}

void LiveEventFeed::setCameras(const QVector<fovea::Camera>& cameras) {
  QHash<QString, QString> labels;
  for (const fovea::Camera& c : cameras) labels.insert(c.id, c.code.isEmpty() ? c.name : c.code);
  if (labels == cameraLabels_) return;
  cameraLabels_ = labels;
  rebuild();
}

// Following the newest unresolved event never moves a selection that is still
// unresolved, so Acknowledge acts on the row the operator has been looking at.
void LiveEventFeed::rebuild() {
  QVector<FeedRow> rows;
  const QVector<EventInfo> none;
  for (const EventInfo& e : store_.eventsStale() ? none : store_.events()) {
    if (unresolvedOnly_ && alertBucket(e) != AlertBucket::Unresolved) continue;
    rows.push_back({e, cameraLabels_.value(e.cameraId)});
  }
  model_->setRows(std::move(rows));
  const int current = model_->indexOf(selectedId_);
  const bool keep = current >= 0 && (!followNewest_ || alertBucket(model_->rowAt(current).event) == AlertBucket::Unresolved);
  QString target = keep ? selectedId_ : QString();
  if (target.isEmpty()) {
    for (int i = 0; i < model_->rowCount(); ++i) {
      if (alertBucket(model_->rowAt(i).event) == AlertBucket::Unresolved) {
        target = model_->rowAt(i).event.id;
        break;
      }
    }
  }
  select(target);
  if (!store_.eventsLoaded() || store_.eventsStale())
    empty_->setText(store_.eventsError().isEmpty() ? QStringLiteral("Loading events…")
                                                  : QStringLiteral("Events unavailable · %1").arg(store_.eventsError()));
  else if (model_->rowCount() == 0)
    empty_->setText(unresolvedOnly_ ? QStringLiteral("No unresolved events.") : QStringLiteral("No events in the last 7 days."));
  else empty_->clear();
  empty_->setVisible(!empty_->text().isEmpty());
}

void LiveEventFeed::select(const QString& eventId) {
  selectedId_ = eventId;
  const int row = model_->indexOf(eventId);
  if (row >= 0) list_->selectionModel()->setCurrentIndex(model_->index(row), QItemSelectionModel::ClearAndSelect);
  else list_->selectionModel()->clearSelection();
  updateButtons();
}

void LiveEventFeed::updateButtons() {
  const int row = model_->indexOf(selectedId_);
  const bool has = row >= 0;
  acknowledge_->setEnabled(has && model_->rowAt(row).event.operatorState == QLatin1StringView("new"));
  openCase_->setEnabled(has);
}

}

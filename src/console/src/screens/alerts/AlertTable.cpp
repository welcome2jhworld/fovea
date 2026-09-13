#include "screens/alerts/AlertTable.h"
#include "core/EventStore.h"
#include "core/ThumbnailCache.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "util/Format.h"
#include "widgets/Painting.h"
#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kSidePadding = 20;
constexpr int kColumnGap = 16;
constexpr int kPreviewWidth = 118;
constexpr int kPreviewHeight = 56;
constexpr int kRuleWidth = 150;
constexpr int kCameraWidth = 130;
constexpr int kTimeWidth = 120;
constexpr int kStatusWidth = 130;
constexpr int kMinAlertWidth = 160;
constexpr int kTitleDetailGap = 4;
constexpr int kStatusChipHeight = 22;
constexpr int kStatusChipPaddingX = 8;
constexpr int64_t kDayMs = 24LL * 3600 * 1000;
constexpr double kHeaderSpacingPx = 1.2;

// Fixed columns from the handoff; the Rule and then the Camera column drop out when the alert text would get too narrow.
struct Columns {
  int preview = 0;
  int alert = 0;
  int alertWidth = 0;
  int rule = 0;
  int ruleWidth = 0;
  int camera = 0;
  int cameraWidth = 0;
  int time = 0;
  int status = 0;
};

Columns columnsFor(int left, int width) {
  Columns c;
  c.ruleWidth = kRuleWidth;
  c.cameraWidth = kCameraWidth;
  c.alertWidth = width - 2 * kSidePadding - (kPreviewWidth + kRuleWidth + kCameraWidth + kTimeWidth + kStatusWidth) - 5 * kColumnGap;
  if (c.alertWidth < kMinAlertWidth) {
    c.alertWidth += c.ruleWidth + kColumnGap;
    c.ruleWidth = 0;
  }
  if (c.alertWidth < kMinAlertWidth) {
    c.alertWidth += c.cameraWidth + kColumnGap;
    c.cameraWidth = 0;
  }
  int x = left + kSidePadding;
  c.preview = x;
  x += kPreviewWidth + kColumnGap;
  c.alert = x;
  x += c.alertWidth + kColumnGap;
  c.rule = x;
  if (c.ruleWidth > 0) x += c.ruleWidth + kColumnGap;
  c.camera = x;
  if (c.cameraWidth > 0) x += c.cameraWidth + kColumnGap;
  c.time = x;
  x += kTimeWidth + kColumnGap;
  c.status = x;
  return c;
}

struct StatusChip {
  QString text;
  Tone tone;
};

StatusChip statusChip(const EventInfo& e) {
  if (alertBucket(e) == AlertBucket::Dismissed) {
    if (e.review && e.review->label == QLatin1StringView("false_alarm")) return {QStringLiteral("FALSE ALARM"), Tone::Neutral};
    return {QStringLiteral("RESOLVED"), Tone::Positive};
  }
  return {severityLabel(e.severity), severityTone(e.severity)};
}

bool deliveryFailed(const EventInfo& e) {
  return std::any_of(e.deliveries.begin(), e.deliveries.end(),
                     [](const DeliveryInfo& d) { return d.state == QLatin1StringView("failed"); });
}

int64_t rangeStartUtcMs(int rangeIndex) {
  const int64_t now = QDateTime::currentMSecsSinceEpoch();
  switch (rangeIndex) {
    case 0: return QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch();
    case 1: return now - kDayMs;
    default: return now - EventStore::kEventWindowMs;
  }
}

void drawCover(QPainter& p, const QRectF& target, const QImage& image) {
  const double scale = std::max(target.width() / image.width(), target.height() / image.height());
  const QSizeF source(target.width() / scale, target.height() / scale);
  const QRectF from(QPointF((image.width() - source.width()) / 2.0, (image.height() - source.height()) / 2.0), source);
  p.drawImage(target, image, from);
}
}

AlertTableModel::AlertTableModel(QObject* parent) : QAbstractListModel(parent) {}

void AlertTableModel::setRows(QVector<AlertRow> rows) {
  beginResetModel();
  rows_ = std::move(rows);
  endResetModel();
}

int AlertTableModel::indexOf(const QString& eventId) const {
  for (int i = 0; i < rows_.size(); ++i)
    if (rows_[i].event.id == eventId) return i;
  return -1;
}

int AlertTableModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant AlertTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= rows_.size()) return {};
  if (role == Qt::DisplayRole) return rows_[index.row()].event.title;
  return {};
}

AlertRowDelegate::AlertRowDelegate(ThumbnailCache& thumbnails, QObject* parent)
    : QStyledItemDelegate(parent), thumbnails_(thumbnails) {}

QSize AlertRowDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
  return QSize(option.rect.width(), tk::size::alertRow);
}

void AlertRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
  const auto* model = static_cast<const AlertTableModel*>(index.model());
  const AlertRow& row = model->rowAt(index.row());
  const EventInfo& e = row.event;
  QPainter& p = *painter;
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  const QRect r = option.rect;
  if (option.state & QStyle::State_Selected) p.fillRect(r, tk::color::q(tk::color::bgPanel));
  if (index.row() == 0 && severityTone(e.severity) == Tone::Critical && alertBucket(e) == AlertBucket::Unresolved)
    p.fillRect(r, tk::color::tintCriticalRow());
  p.fillRect(QRect(r.left(), r.bottom(), r.width(), 1), tk::color::q(tk::color::lineRow));
  const Columns c = columnsFor(r.left(), r.width());
  const int rowHeight = r.height() - 1;

  const QRectF preview(c.preview + 0.5, r.top() + (rowHeight - kPreviewHeight) / 2 + 0.5, kPreviewWidth - 1, kPreviewHeight - 1);
  QPainterPath clip;
  clip.addRoundedRect(preview, tk::radius::control, tk::radius::control);
  p.save();
  p.setClipPath(clip);
  p.fillRect(preview, tk::color::q(tk::color::bgVideo));
  const bool deleted = e.evidence && e.evidence->state == QLatin1StringView("deleted");
  const QImage thumb = e.evidence && !deleted ? thumbnails_.thumbnail(e.evidence->id) : QImage();
  if (thumb.isNull()) paintPlaceholderStripes(p, preview);
  else drawCover(p, preview, thumb);
  p.restore();
  p.setPen(QPen(tk::color::q(tk::color::line), 1.0));
  p.setBrush(Qt::NoBrush);
  p.drawPath(clip);

  const QFont titleFont = Theme::sans(tk::font::emphasis, QFont::Medium);
  const QFont detailFont = Theme::small();
  const QFontMetrics titleMetrics(titleFont);
  const QFontMetrics detailMetrics(detailFont);
  const int blockTop = r.top() + (rowHeight - (titleMetrics.height() + kTitleDetailGap + detailMetrics.height())) / 2;
  p.setFont(titleFont);
  p.setPen(tk::color::q(tk::color::textPrimary));
  p.drawText(QRect(c.alert, blockTop, c.alertWidth, titleMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
             titleMetrics.elidedText(e.title, Qt::ElideRight, c.alertWidth));
  p.setFont(detailFont);
  p.setPen(tk::color::q(tk::color::textSecondary));
  p.drawText(QRect(c.alert, blockTop + titleMetrics.height() + kTitleDetailGap, c.alertWidth, detailMetrics.height()),
             Qt::AlignLeft | Qt::AlignVCenter, detailMetrics.elidedText(e.detail, Qt::ElideRight, c.alertWidth));

  const QRect band(0, r.top(), 0, rowHeight);
  auto cell = [&](int x, int w, const QFont& font, const QString& text) {
    if (w <= 0) return;
    p.setFont(font);
    p.setPen(tk::color::q(tk::color::textSecondary));
    p.drawText(QRect(x, band.top(), w, band.height()), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(font).elidedText(text, Qt::ElideRight, w));
  };
  cell(c.rule, c.ruleWidth, detailFont, row.ruleName);
  cell(c.camera, c.cameraWidth, Theme::mono(tk::font::small), row.cameraLabel);
  cell(c.time, kTimeWidth, Theme::mono(tk::font::small), localTimeLabel(e.openedUtcMs));

  const StatusChip chip = statusChip(e);
  const bool failed = deliveryFailed(e);
  const QFont failFont = Theme::sans(tk::font::label);
  const int failHeight = failed ? QFontMetrics(failFont).height() + 2 : 0;
  const int chipTop = r.top() + (rowHeight - kStatusChipHeight - failHeight) / 2;
  paintToneChip(p, QPoint(c.status, chipTop), chip.text, chip.tone, kStatusChipHeight, kStatusChipPaddingX, Theme::monoMicro());
  if (failed) {
    p.setFont(failFont);
    p.setPen(tk::color::q(tk::color::warning));
    p.drawText(QRect(c.status, chipTop + kStatusChipHeight + 2, kStatusWidth, failHeight), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("delivery failed"));
  }
  p.restore();
}

AlertTableHeader::AlertTableHeader(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("AlertTableHeader"));
  setFixedHeight(tk::size::tableHeader);
}

void AlertTableHeader::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(QRect(0, height() - 1, width(), 1), tk::color::q(tk::color::lineQuiet));
  QFont font = Theme::monoMicro();
  font.setLetterSpacing(QFont::AbsoluteSpacing, kHeaderSpacingPx);
  p.setFont(font);
  p.setPen(tk::color::q(tk::color::textMuted));
  const Columns c = columnsFor(0, width());
  const QRect band(0, 0, 0, height() - 1);
  auto label = [&](int x, int w, const char* text) {
    if (w > 0) p.drawText(QRect(x, band.top(), w, band.height()), Qt::AlignLeft | Qt::AlignVCenter, QString::fromLatin1(text));
  };
  label(c.preview, kPreviewWidth, "PREVIEW");
  label(c.alert, c.alertWidth, "ALERT");
  label(c.rule, c.ruleWidth, "RULE");
  label(c.camera, c.cameraWidth, "CAMERA");
  label(c.time, kTimeWidth, "TIME");
  label(c.status, kStatusWidth, "STATUS");
}

AlertTable::AlertTable(EventStore& store, ThumbnailCache& thumbnails, QWidget* parent) : QWidget(parent), store_(store) {
  setObjectName(QStringLiteral("AlertTable"));
  setAttribute(Qt::WA_StyledBackground, true);

  auto* toolbar = new QWidget(this);
  toolbar->setObjectName(QStringLiteral("AlertToolbar"));
  toolbar->setAttribute(Qt::WA_StyledBackground, true);
  toolbar->setFixedHeight(tk::size::toolbar);
  tabGroup_ = new QButtonGroup(this);
  tabGroup_->setExclusive(true);
  auto* toolbarRow = new QHBoxLayout(toolbar);
  toolbarRow->setContentsMargins(kSidePadding, 0, kSidePadding, 0);
  toolbarRow->setSpacing(8);
  auto* tabsRow = new QHBoxLayout();
  tabsRow->setContentsMargins(0, 0, 0, 0);
  tabsRow->setSpacing(6);
  toolbarRow->addLayout(tabsRow);
  for (int i = 0; i < 3; ++i) {
    auto* tab = new QToolButton(toolbar);
    tab->setProperty("role", QStringLiteral("status-tab"));
    tab->setCheckable(true);
    tab->setCursor(Qt::PointingHandCursor);
    tab->setFocusPolicy(Qt::NoFocus);
    tabGroup_->addButton(tab, i);
    tabsRow->addWidget(tab);
    tabs_[static_cast<size_t>(i)] = tab;
  }
  tabs_[0]->setChecked(true);
  toolbarRow->addStretch(1);
  severity_ = new QComboBox(toolbar);
  severity_->setObjectName(QStringLiteral("SeverityFilter"));
  severity_->addItem(QStringLiteral("All severities"), QString());
  severity_->addItem(QStringLiteral("Critical"), QStringLiteral("critical"));
  severity_->addItem(QStringLiteral("Review"), QStringLiteral("review"));
  severity_->addItem(QStringLiteral("Info"), QStringLiteral("info"));
  range_ = new QComboBox(toolbar);
  range_->setObjectName(QStringLiteral("RangeFilter"));
  range_->addItems({QStringLiteral("Today"), QStringLiteral("Last 24 h"), QStringLiteral("Last 7 days")});
  range_->setCurrentIndex(2);
  for (QComboBox* combo : {severity_, range_}) {
    Theme::setVariant(combo, "sm");
    combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    toolbarRow->addWidget(combo);
  }

  auto* header = new AlertTableHeader(this);
  model_ = new AlertTableModel(this);
  list_ = new QListView(this);
  list_->setObjectName(QStringLiteral("AlertList"));
  list_->setModel(model_);
  list_->setItemDelegate(new AlertRowDelegate(thumbnails, list_));
  list_->setFrameShape(QFrame::NoFrame);
  list_->setSelectionMode(QAbstractItemView::SingleSelection);
  list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_->setUniformItemSizes(true);
  list_->viewport()->setAutoFillBackground(false);

  empty_ = new QLabel(this);
  empty_->setObjectName(QStringLiteral("AlertEmpty"));
  empty_->setAlignment(Qt::AlignCenter);
  empty_->setContentsMargins(20, 32, 20, 32);

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(toolbar);
  column->addWidget(header);
  column->addWidget(empty_);
  column->addWidget(list_, 1);

  connect(tabGroup_, &QButtonGroup::idClicked, this, [this](int id) {
    bucket_ = static_cast<AlertBucket>(id);
    rebuild();
  });
  connect(severity_, &QComboBox::currentIndexChanged, this, &AlertTable::rebuild);
  connect(range_, &QComboBox::currentIndexChanged, this, &AlertTable::rebuild);
  connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& current) {
    if (rebuilding_ || !current.isValid()) return;
    const QString id = model_->rowAt(current.row()).event.id;
    if (id == selectedId_) return;
    selectedId_ = id;
    emit selectedEventChanged(id);
  });
  connect(&store_, &EventStore::eventsChanged, this, &AlertTable::rebuild);
  connect(&store_, &EventStore::rulesChanged, this, &AlertTable::rebuild);
  connect(&thumbnails, &ThumbnailCache::thumbnailReady, list_->viewport(), qOverload<>(&QWidget::update));
  rebuild();
}

void AlertTable::setCameras(const QVector<fovea::Camera>& cameras) {
  QHash<QString, QString> labels;
  for (const fovea::Camera& c : cameras) labels.insert(c.id, c.code.isEmpty() ? c.name : c.code);
  if (labels == cameraLabels_) return;
  cameraLabels_ = labels;
  rebuild();
}

bool AlertTable::passesFilters(const EventInfo& event) const {
  const QString severity = severity_->currentData().toString();
  if (!severity.isEmpty() && event.severity != severity) return false;
  return event.openedUtcMs >= rangeStartUtcMs(range_->currentIndex()) || store_.isPinned(event.id);
}

void AlertTable::rebuild() {
  std::array<int, 3> counts{};
  QVector<AlertRow> rows;
  const QVector<EventInfo> none;
  for (const EventInfo& e : store_.eventsStale() ? none : store_.events()) {
    if (!passesFilters(e)) continue;
    const AlertBucket bucket = alertBucket(e);
    ++counts[static_cast<size_t>(bucket)];
    if (bucket != bucket_) continue;
    rows.push_back({e, store_.ruleLabel(e.ruleId), cameraLabels_.value(e.cameraId)});
  }
  const QString more = store_.eventsSaturated() ? QStringLiteral("+") : QString();
  const char* const names[] = {"Unresolved", "Acknowledged", "Dismissed"};
  for (size_t i = 0; i < tabs_.size(); ++i)
    tabs_[i]->setText(QStringLiteral("%1 · %2%3").arg(QLatin1StringView(names[i])).arg(counts[i]).arg(more));

  rebuilding_ = true;
  model_->setRows(std::move(rows));
  const int selected = model_->indexOf(selectedId_);
  if (selected >= 0) list_->selectionModel()->setCurrentIndex(model_->index(selected), QItemSelectionModel::ClearAndSelect);
  rebuilding_ = false;

  const char* const emptyText[] = {"No unresolved alerts in this range.", "No acknowledged alerts in this range.",
                                   "No dismissed alerts in this range."};
  if (!store_.eventsLoaded() || store_.eventsStale())
    empty_->setText(store_.eventsError().isEmpty() ? QStringLiteral("Loading alerts…")
                                                  : QStringLiteral("Alerts unavailable · %1").arg(store_.eventsError()));
  else if (model_->rowCount() == 0) empty_->setText(QString::fromLatin1(emptyText[static_cast<size_t>(bucket_)]));
  else empty_->clear();
  empty_->setVisible(!empty_->text().isEmpty());
}

void AlertTable::selectEvent(const QString& eventId) {
  selectedId_ = eventId;
  if (const EventInfo* e = store_.findEvent(eventId)) {
    if (!passesFilters(*e)) {
      const QSignalBlocker blockSeverity(severity_);
      const QSignalBlocker blockRange(range_);
      severity_->setCurrentIndex(0);
      range_->setCurrentIndex(2);
    }
    bucket_ = alertBucket(*e);
    tabs_[static_cast<size_t>(bucket_)]->setChecked(true);
  }
  rebuild();
  const int row = model_->indexOf(eventId);
  if (row >= 0) list_->scrollTo(model_->index(row));
  emit selectedEventChanged(eventId);
}

}

#include "screens/search/SearchScreen.h"
#include "core/CoreClient.h"
#include "core/ThumbnailCache.h"
#include "fovea/Clock.h"
#include "screens/search/CustomRangePopup.h"
#include "screens/search/ResultGrid.h"
#include "screens/search/SearchInspector.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "widgets/FormField.h"
#include "widgets/SelectButton.h"
#include <QActionGroup>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr QMargins kQueryBlockMargins(32, 24, 32, 18);
constexpr int kQueryBlockGap = 14;
constexpr int kQueryBarHeight = 56;
constexpr QMargins kQueryBarMargins(18, 0, 8, 0);
constexpr int kQueryBarGap = 14;
constexpr int kFilterGap = 8;
constexpr int kResultsPaddingX = 24;
constexpr int kResultsPaddingTop = 20;
constexpr int kResultsGap = 16;
constexpr int kHeaderGap = 12;
constexpr int kMessageWidth = 460;
constexpr int kMessageGap = 8;

struct RangeChoice {
  RangePreset preset;
  const char* label;
};
constexpr RangeChoice kRangeChoices[] = {{RangePreset::LastHour, "Last 1 hour"},
                                         {RangePreset::LastDay, "Last 24 hours"},
                                         {RangePreset::LastWeek, "Last 7 days"},
                                         {RangePreset::Custom, "Custom range…"}};
}

SearchScreen::SearchScreen(CoreClient& client, ThumbnailCache& thumbnails, QWidget* parent)
    : QWidget(parent), client_(client), thumbnails_(&thumbnails) {
  setObjectName(QStringLiteral("SearchScreen"));
  setAttribute(Qt::WA_StyledBackground, true);

  model_ = new SearchResultsModel(this);
  inspector_ = new SearchInspector(client, thumbnails, this);
  QWidget* queryBlock = buildQueryBlock();
  QWidget* results = buildResultsArea(thumbnails);

  auto* content = new QWidget(this);
  auto* row = new QHBoxLayout(content);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(0);
  row->addWidget(results, 1);
  row->addWidget(inspector_);

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(queryBlock);
  column->addWidget(content, 1);

  rangeMenu_ = new QMenu(this);
  auto* rangeGroup = new QActionGroup(rangeMenu_);
  for (const RangeChoice& choice : kRangeChoices) {
    QAction* action = rangeMenu_->addAction(QString::fromUtf8(choice.label));
    action->setCheckable(true);
    action->setData(static_cast<int>(choice.preset));
    rangeGroup->addAction(action);
    connect(action, &QAction::triggered, this, [this, preset = choice.preset] {
      if (preset != RangePreset::Custom) {
        const bool changed = preset != preset_;
        setRangePreset(preset);
        if (changed) filtersChanged();
        return;
      }
      const QSignalBlocker block(rangeChip_);
      rangeChip_->setChecked(true);
      rangePopup_->showBelow(rangeChip_, rangeFor(preset_, fovea::utcNowMs(), customRange_));
    });
  }
  connect(rangeMenu_, &QMenu::aboutToHide, rangeChip_, [this] { rangeChip_->setChecked(false); });
  connect(rangeChip_, &QAbstractButton::toggled, this, [this](bool open) {
    if (open) showRangeMenu();
  });

  rangePopup_ = new CustomRangePopup(this);
  connect(rangePopup_, &CustomRangePopup::applied, this, [this](const TimeRange& range) {
    customRange_ = range;
    preset_ = RangePreset::Custom;
    updateFilterLabels();
    filtersChanged();
  });
  connect(rangePopup_, &CustomRangePopup::closed, rangeChip_, [this] { rangeChip_->setChecked(false); });

  cameraMenu_ = new QMenu(this);
  cameraMenu_->installEventFilter(this);
  connect(cameraMenu_, &QMenu::aboutToHide, this, [this] {
    cameraChip_->setChecked(false);
    if (selectedCameras_ != selectionAtMenuOpen_) filtersChanged();
  });
  connect(cameraChip_, &QAbstractButton::toggled, this, [this](bool open) {
    if (open) showCameraMenu();
  });

  connect(query_, &QLineEdit::returnPressed, this, &SearchScreen::submit);
  connect(searchButton_, &QPushButton::clicked, this, &SearchScreen::submit);
  connect(messageAction_, &QPushButton::clicked, this, &SearchScreen::runMessageAction);
  connect(grid_, &ResultGrid::playbackToggleRequested, inspector_, &SearchInspector::togglePlayback);
  connect(grid_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& current) {
    if (!current.isValid() || model_->isSkeleton()) {
      inspector_->setResult(std::nullopt, QString(), QString());
      return;
    }
    inspector_->setResult(model_->rowAt(current.row()), indexVersionLabel(response_), response_.model);
  });
  connect(&thumbnails, &ThumbnailCache::thumbnailReady, grid_->viewport(), [this] { grid_->viewport()->update(); });

  indexTimer_.setInterval(kIndexRefreshMs);
  connect(&indexTimer_, &QTimer::timeout, this, &SearchScreen::refreshIndex);

  const QString shortcut = QKeySequence(Qt::CTRL | Qt::Key_K).toString(QKeySequence::NativeText);
  showMessage(QStringLiteral("Describe a moment to search the recorded footage."),
              QStringLiteral("Enter runs the search. %1 brings you back to the query from any screen.").arg(shortcut), false,
              QString());
  updateFilterLabels();
}

QWidget* SearchScreen::buildQueryBlock() {
  auto* block = new QWidget(this);
  block->setObjectName(QStringLiteral("SearchQueryBlock"));
  block->setAttribute(Qt::WA_StyledBackground, true);

  queryBar_ = new QWidget(block);
  queryBar_->setObjectName(QStringLiteral("QueryBar"));
  queryBar_->setAttribute(Qt::WA_StyledBackground, true);
  queryBar_->setFixedHeight(kQueryBarHeight);
  auto* ask = new QLabel(QStringLiteral("ASK"), queryBar_);
  ask->setObjectName(QStringLiteral("QueryAsk"));
  ask->setFont(Theme::mono(tk::font::small));
  query_ = new QLineEdit(queryBar_);
  query_->setObjectName(QStringLiteral("QueryField"));
  query_->setPlaceholderText(QStringLiteral("Describe a moment, for example: person with a bicycle at the north gate"));
  query_->setClearButtonEnabled(false);
  query_->installEventFilter(this);
  searchButton_ = new QPushButton(QStringLiteral("Search"), queryBar_);
  searchButton_->setObjectName(QStringLiteral("SearchButton"));
  searchButton_->setProperty("role", QStringLiteral("primary"));
  searchButton_->setCursor(Qt::PointingHandCursor);
  auto* bar = new QHBoxLayout(queryBar_);
  bar->setContentsMargins(kQueryBarMargins);
  bar->setSpacing(kQueryBarGap);
  bar->addWidget(ask);
  bar->addWidget(query_, 1);
  bar->addWidget(searchButton_);

  rangeChip_ = new SelectButton(block);
  rangeChip_->setObjectName(QStringLiteral("RangeChip"));
  rangeChip_->setToolTip(QStringLiteral("Time range to search"));
  cameraChip_ = new SelectButton(block);
  cameraChip_->setObjectName(QStringLiteral("CameraChip"));
  cameraChip_->setToolTip(QStringLiteral("Cameras to search"));
  stats_ = new QLabel(block);
  stats_->setObjectName(QStringLiteral("SearchStats"));
  stats_->setFont(Theme::mono(tk::font::small));
  stats_->setTextFormat(Qt::RichText);
  auto* filters = new QHBoxLayout();
  filters->setContentsMargins(0, 0, 0, 0);
  filters->setSpacing(kFilterGap);
  filters->addWidget(rangeChip_);
  filters->addWidget(cameraChip_);
  filters->addStretch(1);
  filters->addWidget(stats_);

  indexLine_ = new QLabel(block);
  indexLine_->setObjectName(QStringLiteral("IndexStatus"));
  indexLine_->setFont(Theme::mono(tk::font::label));
  indexLine_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  indexLine_->setFixedHeight(QFontMetrics(indexLine_->font()).height());
  indexLine_->installEventFilter(this);

  auto* filterBlock = new QVBoxLayout();
  filterBlock->setContentsMargins(0, 0, 0, 0);
  filterBlock->setSpacing(kFilterGap);
  filterBlock->addLayout(filters);
  filterBlock->addWidget(indexLine_);

  auto* column = new QVBoxLayout(block);
  column->setContentsMargins(kQueryBlockMargins);
  column->setSpacing(kQueryBlockGap);
  column->addWidget(queryBar_);
  column->addLayout(filterBlock);
  return block;
}

QWidget* SearchScreen::buildResultsArea(ThumbnailCache& thumbnails) {
  auto* area = new QWidget(this);
  area->setObjectName(QStringLiteral("SearchResults"));
  area->setAttribute(Qt::WA_StyledBackground, true);

  auto* label = new QLabel(QStringLiteral("RESULTS"), area);
  label->setProperty("role", QStringLiteral("section-label"));
  label->setFont(Theme::monoLabel());
  auto* rule = new QWidget(area);
  rule->setObjectName(QStringLiteral("ResultsRule"));
  rule->setAttribute(Qt::WA_StyledBackground, true);
  rule->setFixedHeight(1);
  auto* sorted = new QLabel(QStringLiteral("Sorted by relevance"), area);
  sorted->setObjectName(QStringLiteral("ResultsSorted"));
  sorted->setToolTip(QStringLiteral("Embedding similarity to the query; not a probability"));
  auto* header = new QHBoxLayout();
  header->setContentsMargins(0, 0, ResultCardDelegate::kGap, 0);
  header->setSpacing(kHeaderGap);
  header->addWidget(label);
  header->addWidget(rule, 1, Qt::AlignVCenter);
  header->addWidget(sorted);

  body_ = new QStackedWidget(area);
  grid_ = new ResultGrid(body_);
  grid_->setModel(model_);
  grid_->setItemDelegate(new ResultCardDelegate(thumbnails, grid_));
  body_->addWidget(grid_);

  auto* message = new QWidget(body_);
  auto* text = new QWidget(message);
  text->setFixedWidth(kMessageWidth);
  messageSentence_ = new QLabel(text);
  messageSentence_->setObjectName(QStringLiteral("SearchMessage"));
  messageDetail_ = new QLabel(text);
  messageDetail_->setObjectName(QStringLiteral("SearchMessageDetail"));
  auto* textColumn = new QVBoxLayout(text);
  textColumn->setContentsMargins(0, 0, 0, 0);
  textColumn->setSpacing(kMessageGap);
  for (QLabel* line : {messageSentence_, messageDetail_}) {
    line->setAlignment(Qt::AlignCenter);
    line->setWordWrap(true);
    line->setTextInteractionFlags(Qt::TextSelectableByMouse);
    textColumn->addWidget(line);
  }
  messageAction_ = new QPushButton(message);
  messageAction_->setObjectName(QStringLiteral("SearchMessageAction"));
  messageAction_->setProperty("role", QStringLiteral("secondary"));
  Theme::setVariant(messageAction_, "lg");
  messageAction_->setCursor(Qt::PointingHandCursor);
  auto* messageColumn = new QVBoxLayout(message);
  messageColumn->setContentsMargins(0, 0, ResultCardDelegate::kGap, 0);
  messageColumn->setSpacing(kMessageGap * 2);
  messageColumn->addStretch(1);
  messageColumn->addWidget(text, 0, Qt::AlignHCenter);
  messageColumn->addWidget(messageAction_, 0, Qt::AlignHCenter);
  messageColumn->addStretch(2);
  body_->addWidget(message);

  auto* column = new QVBoxLayout(area);
  column->setContentsMargins(kResultsPaddingX, kResultsPaddingTop, kResultsPaddingX - ResultCardDelegate::kGap, 0);
  column->setSpacing(kResultsGap);
  column->addLayout(header);
  column->addWidget(body_, 1);
  return area;
}

void SearchScreen::setCameras(const QVector<fovea::Camera>& cameras) {
  cameras_ = cameras;
  QSet<QString> known;
  for (const fovea::Camera& c : cameras_) known.insert(c.id);
  selectedCameras_.intersect(known);
  if (selectedCameras_.size() >= known.size()) selectedCameras_.clear();
  updateFilterLabels();
  if (indexStatus_) renderIndex();
}

void SearchScreen::focusQuery() {
  query_->setFocus(Qt::ShortcutFocusReason);
  query_->selectAll();
}

void SearchScreen::search(const QString& query) {
  query_->setText(query);
  submit();
}

void SearchScreen::setRangePreset(RangePreset preset) {
  preset_ = preset;
  updateFilterLabels();
}

void SearchScreen::setCameraFilter(const QSet<QString>& cameraIds) {
  selectedCameras_ = cameraIds;
  if (!cameras_.isEmpty() && selectedCameras_.size() >= cameras_.size()) selectedCameras_.clear();
  updateFilterLabels();
}

void SearchScreen::submit() {
  const QString text = query_->text().simplified();
  if (text.isEmpty()) {
    query_->setFocus(Qt::OtherFocusReason);
    return;
  }
  lastQuery_ = text;
  runSearch();
}

// A search that no view will paint is dropped: its reply and the thumbnails of
// the results it replaces would otherwise hold the connections the next query
// needs, and an unfinished reply keeps the console from exiting.
SearchScreen::~SearchScreen() { cancelSearch(); }

void SearchScreen::cancelSearch() {
  ++generation_;
  if (activeSearch_) activeSearch_->abort();
  activeSearch_.clear();
  if (thumbnails_) thumbnails_->cancelPending();
}

// The generation moves before the abort, so the aborted reply's callback is ignored.
void SearchScreen::runSearch() {
  cancelSearch();
  const TimeRange range = rangeFor(preset_, fovea::utcNowMs(), customRange_);
  const QJsonObject request{{"query", lastQuery_},
                            {"from_utc_ms", static_cast<double>(range.fromUtcMs)},
                            {"to_utc_ms", static_cast<double>(range.toUtcMs)},
                            {"camera_ids", requestCameraIds()},
                            {"limit", kResultLimit},
                            {"min_gap_ms", kMinGapMs}};
  response_ = SearchResponseInfo{};
  model_->setSkeleton(kSkeletonCards);
  inspector_->setResult(std::nullopt, QString(), QString());
  body_->setCurrentIndex(GridPage);
  setStats(QStringLiteral("searching…"), nullptr);
  setState(State::Searching);
  const int generation = generation_;
  activeSearch_ = client_.search(request, [this, generation](bool ok, const QJsonDocument& doc, const QString& error) {
    onSearchReply(generation, ok, doc, error);
  }, this);
}

void SearchScreen::onSearchReply(int generation, bool ok, const QJsonDocument& doc, const QString& error) {
  if (generation != generation_) return;
  activeSearch_.clear();
  if (!ok) {
    model_->setRows({});
    setStats(QString(), nullptr);
    showMessage(QStringLiteral("The search did not complete."), error, true, QStringLiteral("Retry"));
    setState(State::Error);
    return;
  }
  response_ = SearchResponseInfo::fromJson(doc.object());
  QVector<SearchResultRow> rows;
  rows.reserve(response_.results.size());
  for (const SearchResultInfo& r : std::as_const(response_.results))
    rows.push_back({r, cameraName(r.cameraId), cameraCode(r.cameraId)});
  model_->setRows(rows);
  setStats(statsLine(static_cast<int>(rows.size()), response_.stats), &response_.stats);
  if (rows.isEmpty()) {
    emptyAction_ = emptyActionFor(preset_, !selectedCameras_.isEmpty());
    showMessage(emptySentence(response_.stats), QString(), false, emptyActionLabel(emptyAction_));
    setState(State::Empty);
    return;
  }
  body_->setCurrentIndex(GridPage);
  const bool typing = query_->hasFocus();
  grid_->setCurrentIndex(model_->index(0, 0));
  if (typing) grid_->setFocus(Qt::OtherFocusReason);
  setState(State::Results);
}

void SearchScreen::setState(State state) {
  if (state_ == state) return;
  state_ = state;
  emit stateChanged(state);
}

void SearchScreen::showMessage(const QString& sentence, const QString& detail, bool detailIsError, const QString& action) {
  messageSentence_->setText(sentence);
  messageDetail_->setText(detail);
  messageDetail_->setVisible(!detail.isEmpty());
  messageDetail_->setProperty("tone", detailIsError ? QStringLiteral("critical") : QString());
  restyle(messageDetail_);
  messageAction_->setText(action);
  messageAction_->setVisible(!action.isEmpty());
  body_->setCurrentIndex(MessagePage);
}

void SearchScreen::runMessageAction() {
  if (state_ == State::Error) {
    runSearch();
    return;
  }
  switch (emptyAction_) {
    case EmptyAction::WidenRange:
      setRangePreset(RangePreset::LastWeek);
      runSearch();
      return;
    case EmptyAction::AllCameras:
      setCameraFilter({});
      runSearch();
      return;
    case EmptyAction::EditQuery:
      focusQuery();
      return;
  }
}

// Coverage below 100 % is drawn in the warning colour, and the tooltip says what was left out.
void SearchScreen::setStats(const QString& text, const SearchStatsInfo* stats) {
  QString html = text.toHtmlEscaped();
  QString tooltip;
  if (stats && coverageIncomplete(*stats)) {
    const QString coverage = QStringLiteral("coverage %1%").arg(coveragePercent(*stats->coverageRatio));
    html.replace(coverage, QStringLiteral("<span style=\"color:%1\">%2</span>").arg(QString(tk::color::warning), coverage));
    tooltip = coverageExplanation(*stats);
  }
  stats_->setText(html);
  stats_->setToolTip(tooltip);
}

void SearchScreen::refreshIndex() {
  if (indexInFlight_) return;
  indexInFlight_ = true;
  client_.indexStatus([this](bool ok, const QJsonDocument& doc, const QString& error) {
    indexInFlight_ = false;
    if (ok) {
      indexStatus_ = IndexStatusInfo::fromJson(doc.object());
      indexError_.clear();
    } else {
      indexStatus_.reset();
      indexError_ = error;
    }
    renderIndex();
  }, this);
}

void SearchScreen::renderIndex() {
  if (indexStatus_) {
    QHash<QString, QString> names;
    for (const fovea::Camera& c : std::as_const(cameras_)) names.insert(c.id, c.name);
    indexText_ = indexStatusLine(*indexStatus_, names);
  } else {
    indexText_ = indexError_.isEmpty() ? QString() : QStringLiteral("Index status unavailable: %1").arg(indexError_);
  }
  const QString lastError = indexStatus_ ? indexStatus_->lastError : QString();
  indexLine_->setToolTip(lastError.isEmpty() ? indexText_ : QStringLiteral("%1\nLast indexing error: %2").arg(indexText_, lastError));
  elideIndexLine();
}

void SearchScreen::elideIndexLine() {
  indexLine_->setText(QFontMetrics(indexLine_->font()).elidedText(indexText_, Qt::ElideRight, indexLine_->width()));
}

void SearchScreen::showRangeMenu() {
  for (QAction* action : rangeMenu_->actions())
    action->setChecked(static_cast<RangePreset>(action->data().toInt()) == preset_);
  rangeMenu_->popup(rangeChip_->mapToGlobal(QPoint(0, rangeChip_->height() + tk::size::popupOffset)));
}

// Camera rows toggle without closing the menu; the query reruns once the menu closes.
void SearchScreen::showCameraMenu() {
  selectionAtMenuOpen_ = selectedCameras_;
  cameraMenu_->clear();
  QAction* all = cameraMenu_->addAction(QStringLiteral("All cameras"));
  all->setCheckable(true);
  cameraMenu_->addSeparator();
  QVector<QAction*> rows;
  for (const fovea::Camera& c : std::as_const(cameras_)) {
    QAction* action = cameraMenu_->addAction(c.code.isEmpty() ? c.name : QStringLiteral("%1 · %2").arg(c.code, c.name));
    action->setCheckable(true);
    action->setData(c.id);
    rows.push_back(action);
  }
  auto sync = [this, all, rows] {
    all->setChecked(selectedCameras_.isEmpty());
    for (QAction* action : rows) action->setChecked(selectedCameras_.contains(action->data().toString()));
    updateFilterLabels();
  };
  connect(all, &QAction::triggered, this, [this, sync] {
    selectedCameras_.clear();
    sync();
  });
  for (QAction* action : rows) {
    connect(action, &QAction::triggered, this, [this, action, sync](bool on) {
      const QString id = action->data().toString();
      if (on) selectedCameras_.insert(id);
      else selectedCameras_.remove(id);
      if (selectedCameras_.size() >= cameras_.size()) selectedCameras_.clear();
      sync();
    });
  }
  if (cameras_.isEmpty()) cameraMenu_->addAction(QStringLiteral("No cameras yet"))->setEnabled(false);
  sync();
  cameraMenu_->popup(cameraChip_->mapToGlobal(QPoint(0, cameraChip_->height() + tk::size::popupOffset)));
}

void SearchScreen::updateFilterLabels() {
  rangeChip_->setLabel(rangeLabel(preset_, customRange_));
  const QString single = selectedCameras_.size() == 1 ? cameraName(*selectedCameras_.cbegin()) : QString();
  cameraChip_->setLabel(cameraFilterLabel(static_cast<int>(selectedCameras_.size()), static_cast<int>(cameras_.size()), single));
}

void SearchScreen::filtersChanged() {
  if (!lastQuery_.isEmpty()) runSearch();
}

// Every known camera when the filter is empty, so the request never depends on how the service reads [].
QJsonArray SearchScreen::requestCameraIds() const {
  QJsonArray ids;
  for (const fovea::Camera& c : cameras_)
    if (selectedCameras_.isEmpty() || selectedCameras_.contains(c.id)) ids.push_back(c.id);
  return ids;
}

QString SearchScreen::cameraName(const QString& id) const {
  for (const fovea::Camera& c : cameras_)
    if (c.id == id) return c.name;
  return QStringLiteral("Camera %1").arg(id.left(8));
}

QString SearchScreen::cameraCode(const QString& id) const {
  for (const fovea::Camera& c : cameras_)
    if (c.id == id) return c.code;
  return QString();
}

bool SearchScreen::eventFilter(QObject* watched, QEvent* event) {
  if (watched == cameraMenu_ && event->type() == QEvent::MouseButtonRelease) {
    QAction* action = cameraMenu_->actionAt(static_cast<QMouseEvent*>(event)->position().toPoint());
    if (action && action->isEnabled() && action->isCheckable()) {
      action->trigger();
      return true;
    }
  }
  if (watched == query_) {
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
      queryBar_->setProperty("focus", event->type() == QEvent::FocusIn);
      restyle(queryBar_);
    } else if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Down &&
               state_ == State::Results) {
      if (!grid_->currentIndex().isValid()) grid_->setCurrentIndex(model_->index(0, 0));
      grid_->setFocus(Qt::OtherFocusReason);
      return true;
    }
  }
  if (watched == indexLine_ && event->type() == QEvent::Resize) elideIndexLine();
  return QWidget::eventFilter(watched, event);
}

void SearchScreen::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  refreshIndex();
  indexTimer_.start();
}

void SearchScreen::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  indexTimer_.stop();
  if (activeSearch_) cancelSearch();
}

}

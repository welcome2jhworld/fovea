#include "screens/alerts/RulesRail.h"
#include "alerts/AlertLogic.h"
#include "core/EventStore.h"
#include "screens/alerts/RuleEditor.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "widgets/FormField.h"
#include "widgets/Painting.h"
#include "widgets/ToggleSwitch.h"
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kCardPadding = 12;
constexpr int kCardGap = 8;
constexpr int kListPadding = 8;
constexpr int kListGap = 4;
constexpr int kChipHeight = 19;
constexpr int kChipPaddingX = 6;
constexpr int kChipGap = 6;
constexpr int kPagerButton = 26;
constexpr int kPagerWindow = 5;

int64_t startOfTodayUtcMs() {
  return QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch();
}
}

RuleChips::RuleChips(QWidget* parent) : QWidget(parent) { setFixedHeight(kChipHeight); }

void RuleChips::set(const QString& scope, const QString& severity, const QString& hits) {
  scope_ = scope;
  severity_ = severity;
  hits_ = hits;
  update();
}

void RuleChips::paintEvent(QPaintEvent*) {
  QPainter p(this);
  const QFont font = Theme::monoMicro();
  int x = 0;
  if (!scope_.isEmpty()) {
    x = paintOutlineChip(p, QPoint(0, 0), scope_, kChipHeight, kChipPaddingX, font, tk::color::q(tk::color::line),
                         tk::color::q(tk::color::textMuted)).right() + 1 + kChipGap;
  }
  paintToneChip(p, QPoint(x, 0), severityLabel(severity_), severityTone(severity_), kChipHeight, kChipPaddingX, font);
  p.setFont(font);
  p.setPen(tk::color::q(tk::color::textDisabled));
  p.drawText(rect(), Qt::AlignVCenter | Qt::AlignRight, hits_);
}

RuleCard::RuleCard(CoreClient& client, EventStore& store, QWidget* parent)
    : QWidget(parent), client_(client), store_(store) {
  name_ = new QLabel(this);
  name_->setObjectName(QStringLiteral("RuleCardName"));
  name_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  edit_ = new QPushButton(QStringLiteral("Edit"), this);
  edit_->setObjectName(QStringLiteral("RuleEdit"));
  edit_->setCursor(Qt::PointingHandCursor);
  edit_->setFocusPolicy(Qt::NoFocus);
  toggle_ = new ToggleSwitch(ToggleSwitch::Size::Sm, this);
  toggle_->setToolTip(QStringLiteral("Enable or disable the rule"));
  toggle_->setChecked(true);
  auto* header = new QHBoxLayout();
  header->setContentsMargins(0, 0, 0, 0);
  header->setSpacing(10);
  header->addWidget(name_, 1);
  header->addWidget(edit_);
  header->addWidget(toggle_);

  summary_ = new QWidget(this);
  sentence_ = new QLabel(summary_);
  sentence_->setObjectName(QStringLiteral("RuleSentence"));
  sentence_->setWordWrap(true);
  chips_ = new RuleChips(summary_);
  auto* summaryColumn = new QVBoxLayout(summary_);
  summaryColumn->setContentsMargins(0, 0, 0, 0);
  summaryColumn->setSpacing(kCardGap);
  summaryColumn->addWidget(sentence_);
  summaryColumn->addWidget(chips_);

  column_ = new QVBoxLayout(this);
  column_->setContentsMargins(kCardPadding, kCardPadding, kCardPadding, kCardPadding);
  column_->setSpacing(kCardGap);
  column_->addLayout(header);
  column_->addWidget(summary_);

  connect(edit_, &QPushButton::clicked, this, [this] {
    if (!editing_) emit editRequested();
  });
  connect(toggle_, &ToggleSwitch::clicked, this, [this](bool on) {
    if (rule_) store_.setRuleEnabled(rule_->id, on);
  });
}

void RuleCard::setRule(const std::optional<RuleInfo>& rule, const QString& sentence, const QString& scope,
                       const QString& hits) {
  rule_ = rule;
  name_->setText(rule ? rule->name : QStringLiteral("New rule"));
  sentence_->setText(sentence);
  chips_->set(scope, rule ? rule->severity : QString(), hits);
  if (!rule) return;
  const QSignalBlocker block(toggle_);
  toggle_->setChecked(rule->enabled);
}

void RuleCard::setCameras(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses) {
  if (editor_) editor_->setCameras(cameras, statuses);
}

// The editor is built on first use: most cards are never edited in a session.
void RuleCard::setEditing(bool editing) {
  if (editing == editing_) return;
  editing_ = editing;
  if (editing && !editor_) {
    editor_ = new RuleEditor(client_, store_, this);
    editor_->setEnabledSource([this] { return toggle_->isChecked(); });
    column_->addWidget(editor_);
    connect(editor_, &RuleEditor::saved, this, [this](const QString& id) { emit editorClosed(id); });
    connect(editor_, &RuleEditor::cancelled, this, [this] { emit editorClosed(QString()); });
  }
  if (editing) editor_->load(rule_);
  if (editor_) editor_->setVisible(editing);
  summary_->setVisible(!editing && rule_);
  edit_->setText(editing ? QStringLiteral("Editing") : QStringLiteral("Edit"));
  edit_->setProperty("editing", editing);
  restyle(edit_);
  update();
}

void RuleCard::paintEvent(QPaintEvent*) {
  if (!editing_) return;
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(QPen(tk::color::q(tk::color::lineStrong), 1.0));
  p.setBrush(tk::color::q(tk::color::bgPanel));
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), tk::radius::listCard, tk::radius::listCard);
}

RulesRail::RulesRail(CoreClient& client, EventStore& store, QWidget* parent)
    : QWidget(parent), client_(client), store_(store) {
  setObjectName(QStringLiteral("RulesRail"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedWidth(tk::size::rulesRail);

  auto* header = new QWidget(this);
  header->setObjectName(QStringLiteral("RulesHeader"));
  header->setAttribute(Qt::WA_StyledBackground, true);
  header->setFixedHeight(tk::size::panelHeader);
  auto* title = new QLabel(QStringLiteral("RULES"), header);
  title->setProperty("role", QStringLiteral("section-label"));
  title->setFont(Theme::monoLabel());
  counts_ = new QLabel(header);
  counts_->setObjectName(QStringLiteral("RulesCounts"));
  counts_->setFont(Theme::mono(tk::font::label));
  auto* add = new QPushButton(QStringLiteral("＋ New rule"), header);
  add->setObjectName(QStringLiteral("NewRule"));
  add->setProperty("role", QStringLiteral("primary"));
  Theme::setVariant(add, "sm");
  add->setCursor(Qt::PointingHandCursor);
  auto* headerRow = new QHBoxLayout(header);
  headerRow->setContentsMargins(16, 0, 12, 0);
  headerRow->setSpacing(10);
  headerRow->addWidget(title);
  headerRow->addWidget(counts_);
  headerRow->addStretch(1);
  headerRow->addWidget(add);

  auto* content = new QWidget();
  content->setObjectName(QStringLiteral("RulesList"));
  list_ = new QVBoxLayout(content);
  list_->setContentsMargins(kListPadding, kListPadding, kListPadding, kListPadding);
  list_->setSpacing(kListGap);
  scroll_ = new QScrollArea(this);
  scroll_->setObjectName(QStringLiteral("RulesScroll"));
  scroll_->setWidget(content);
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  empty_ = new QLabel(this);
  empty_->setObjectName(QStringLiteral("RulesEmpty"));
  empty_->setAlignment(Qt::AlignCenter);
  empty_->setWordWrap(true);
  empty_->setContentsMargins(16, 24, 16, 24);

  auto* footer = new QWidget(this);
  footer->setObjectName(QStringLiteral("RulesFooter"));
  footer->setAttribute(Qt::WA_StyledBackground, true);
  footer->setFixedHeight(tk::size::railFooter);
  range_ = new QLabel(footer);
  range_->setObjectName(QStringLiteral("RulesRange"));
  range_->setFont(Theme::mono(tk::font::label));
  pager_ = new QWidget(footer);
  auto* pagerRow = new QHBoxLayout(pager_);
  pagerRow->setContentsMargins(0, 0, 0, 0);
  pagerRow->setSpacing(4);
  auto* footerRow = new QHBoxLayout(footer);
  footerRow->setContentsMargins(16, 0, 12, 0);
  footerRow->addWidget(range_);
  footerRow->addStretch(1);
  footerRow->addWidget(pager_);

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(header);
  column->addWidget(empty_);
  column->addWidget(scroll_, 1);
  column->addWidget(footer);

  connect(add, &QPushButton::clicked, this, &RulesRail::newRule);
  connect(&store_, &EventStore::rulesChanged, this, &RulesRail::rebuild);
  connect(&store_, &EventStore::eventsChanged, this, &RulesRail::rebuild);
  rebuild();
}

QString RulesRail::cameraLabel(const QString& cameraId) const {
  for (const fovea::Camera& c : cameras_)
    if (c.id == cameraId) return c.code.isEmpty() ? c.name : c.code;
  return {};
}

void RulesRail::setCameras(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses) {
  const bool labelsChanged = [&] {
    if (cameras.size() != cameras_.size()) return true;
    for (qsizetype i = 0; i < cameras.size(); ++i)
      if (cameras[i].id != cameras_[i].id || cameras[i].code != cameras_[i].code || cameras[i].name != cameras_[i].name)
        return true;
    return false;
  }();
  cameras_ = cameras;
  statuses_ = statuses;
  for (RuleCard* card : std::as_const(cards_))
    if (card->isEditing()) card->setCameras(cameras_, statuses_);
  if (draft_) draft_->setCameras(cameras_, statuses_);
  if (labelsChanged) rebuild();
}

// Stale rules stay listed (an open editor keeps its card) under a header that says the service is unavailable.
void RulesRail::rebuild() {
  const QVector<RuleInfo>& rules = store_.rules();
  const int total = static_cast<int>(rules.size());
  const int active = static_cast<int>(std::count_if(rules.begin(), rules.end(), [](const RuleInfo& r) { return r.enabled; }));
  counts_->setText(store_.rulesStale() ? QStringLiteral("not current · %1").arg(store_.rulesError())
                                  : QStringLiteral("%1 total · %2 active").arg(total).arg(active));

  const int64_t today = startOfTodayUtcMs();
  QHash<QString, int> hits;
  for (const EventInfo& e : store_.events())
    if (e.openedUtcMs >= today) ++hits[e.ruleId];
  const QString saturated = store_.eventsSaturated() ? QStringLiteral("+") : QString();

  QHash<QString, RuleCard*> kept;
  for (const RuleInfo& r : rules) {
    RuleCard* card = cards_.take(r.id);
    if (!card) {
      card = new RuleCard(client_, store_, this);
      const QString id = r.id;
      connect(card, &RuleCard::editRequested, this, [this, id] { editRule(id); });
      connect(card, &RuleCard::editorClosed, this, [this, card](const QString& saved) { closeEditor(card, saved); });
    }
    const ZoneInfo* zone = store_.findZone(r.zoneId);
    const QString camera = cameraLabel(r.cameraId);
    card->setRule(r, triggerSentence(r, zone ? zone->name : QString(), camera),
                  camera.isEmpty() ? QStringLiteral("unknown camera") : camera,
                  QStringLiteral("%1%2 today").arg(hits.value(r.id)).arg(saturated));
    kept.insert(r.id, card);
  }
  for (RuleCard* gone : std::as_const(cards_)) gone->deleteLater();
  cards_ = kept;

  const int pages = std::max(1, (total + kPageSize - 1) / kPageSize);
  page_ = std::clamp(page_, 0, pages - 1);
  const int first = page_ * kPageSize;
  const int last = std::min(total, first + kPageSize);

  while (QLayoutItem* item = list_->takeAt(0)) delete item;
  if (draft_) list_->addWidget(draft_);
  for (int i = 0; i < total; ++i) {
    RuleCard* card = cards_.value(rules[i].id);
    const bool onPage = i >= first && i < last;
    if (onPage) list_->addWidget(card);
    card->setVisible(onPage);
  }
  list_->addStretch(1);

  if (store_.rulesLoaded()) empty_->setText(QStringLiteral("No rules yet. ＋ New rule draws a zone on a camera."));
  else if (store_.rulesError().isEmpty()) empty_->setText(QStringLiteral("Loading rules…"));
  else empty_->setText(QStringLiteral("Rules unavailable · %1").arg(store_.rulesError()));
  empty_->setVisible(total == 0 && !draft_);
  range_->setText(total == 0 ? QStringLiteral("0 rules") : QStringLiteral("%1–%2 of %3").arg(first + 1).arg(last).arg(total));

  auto* pagerRow = static_cast<QHBoxLayout*>(pager_->layout());
  while (QLayoutItem* item = pagerRow->takeAt(0)) {
    item->widget()->deleteLater();
    delete item;
  }
  auto pagerButton = [this, pagerRow](const QString& text, int target, bool enabled, bool current, bool arrow) {
    auto* b = new QToolButton(pager_);
    b->setText(text);
    b->setProperty("role", QStringLiteral("pager"));
    b->setProperty("current", current);
    b->setProperty("arrow", arrow);
    b->setFixedSize(kPagerButton, kPagerButton);
    b->setEnabled(enabled);
    b->setCursor(Qt::PointingHandCursor);
    connect(b, &QToolButton::clicked, this, [this, target] {
      page_ = target;
      rebuild();
    });
    pagerRow->addWidget(b);
  };
  const int windowStart = std::clamp(page_ - kPagerWindow / 2, 0, std::max(0, pages - kPagerWindow));
  pagerButton(QStringLiteral("‹"), page_ - 1, page_ > 0, false, true);
  for (int i = windowStart; i < std::min(pages, windowStart + kPagerWindow); ++i)
    pagerButton(QString::number(i + 1), i, true, i == page_, false);
  pagerButton(QStringLiteral("›"), page_ + 1, page_ < pages - 1, false, true);
}

void RulesRail::editRule(const QString& ruleId) {
  const QVector<RuleInfo>& rules = store_.rules();
  const auto it = std::find_if(rules.begin(), rules.end(), [&](const RuleInfo& r) { return r.id == ruleId; });
  RuleCard* card = cards_.value(ruleId);
  if (it == rules.end() || !card) return;
  if (draft_) closeEditor(draft_, QString());
  for (RuleCard* other : std::as_const(cards_))
    if (other != card) other->setEditing(false);
  page_ = static_cast<int>(std::distance(rules.begin(), it)) / kPageSize;
  rebuild();
  card->setEditing(true);
  card->setCameras(cameras_, statuses_);
  scroll_->ensureWidgetVisible(card, 0, 0);
}

void RulesRail::newRule() {
  if (!draft_) {
    for (RuleCard* card : std::as_const(cards_)) card->setEditing(false);
    draft_ = new RuleCard(client_, store_, this);
    connect(draft_, &RuleCard::editorClosed, this, [this](const QString& saved) { closeEditor(draft_, saved); });
    draft_->setRule(std::nullopt, QString(), QString(), QString());
    draft_->setEditing(true);
    draft_->setCameras(cameras_, statuses_);
    rebuild();
  }
  scroll_->ensureWidgetVisible(draft_, 0, 0);
}

void RulesRail::closeEditor(RuleCard* card, const QString& savedRuleId) {
  if (card == draft_) {
    draft_->deleteLater();
    draft_ = nullptr;
  } else {
    card->setEditing(false);
  }
  rebuild();
  if (!savedRuleId.isEmpty()) store_.refreshRules();
}

}

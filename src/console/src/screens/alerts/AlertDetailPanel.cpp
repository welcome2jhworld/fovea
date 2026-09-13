#include "screens/alerts/AlertDetailPanel.h"
#include "alerts/AlertLogic.h"
#include "core/CoreClient.h"
#include "core/EventStore.h"
#include "screens/alerts/EvidencePlayer.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "widgets/FormField.h"
#include "widgets/ToneChip.h"
#include <QDateTime>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kBodyPadding = 16;
constexpr int kBodyGap = 14;
constexpr int kMetaGap = 8;
constexpr int kMetaPaddingBottom = 7;
constexpr int kHeaderChipHeight = 20;
constexpr int kHeaderChipPaddingX = 7;
constexpr int kMaxActivityLines = 12;
const char* const kMetaKeys[] = {"Alert ID", "Rule", "Camera", "Raised", "Condition", "Status", "Review", "Evidence"};
const char* const kReviewLabels[] = {"confirmed", "false_alarm", "undecided"};

enum MetaRow { MetaAlertId, MetaRule, MetaCamera, MetaRaised, MetaCondition, MetaStatus, MetaReview, MetaEvidence };

QString wallTime(int64_t utcMs, const QString& format) {
  return utcMs > 0 ? QDateTime::fromMSecsSinceEpoch(utcMs).toLocalTime().toString(format) : QStringLiteral("—");
}

QString evidenceReason(const EvidenceInfo& ev) {
  if (!ev.reason.isEmpty()) return ev.reason;
  const QString window = QStringLiteral("%1–%2").arg(wallTime(ev.fromUtcMs, QStringLiteral("HH:mm:ss")),
                                                     wallTime(ev.toUtcMs, QStringLiteral("HH:mm:ss")));
  if (ev.state == QLatin1StringView("pending")) return QStringLiteral("%1 · footage not recorded yet").arg(window);
  if (ev.state == QLatin1StringView("deleted")) return QStringLiteral("footage was deleted");
  return window;
}
}

AlertDetailPanel::AlertDetailPanel(CoreClient& client, EventStore& store, ThumbnailCache& thumbnails, QWidget* parent)
    : QWidget(parent), client_(client), store_(store) {
  setObjectName(QStringLiteral("AlertDetail"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedWidth(tk::size::inspector);

  auto* header = new QWidget(this);
  header->setObjectName(QStringLiteral("DetailHeader"));
  header->setAttribute(Qt::WA_StyledBackground, true);
  header->setFixedHeight(tk::size::panelHeader);
  auto* title = new QLabel(QStringLiteral("SELECTED ALERT"), header);
  title->setProperty("role", QStringLiteral("section-label"));
  title->setFont(Theme::monoLabel());
  severity_ = new ToneChip(kHeaderChipHeight, kHeaderChipPaddingX, header);
  auto* headerRow = new QHBoxLayout(header);
  headerRow->setContentsMargins(kBodyPadding, 0, kBodyPadding, 0);
  headerRow->addWidget(title);
  headerRow->addStretch(1);
  headerRow->addWidget(severity_);

  pages_ = new QStackedWidget(this);
  auto* empty = new QLabel(QStringLiteral("Select an alert to review its evidence and activity."), pages_);
  empty->setObjectName(QStringLiteral("DetailEmpty"));
  empty->setAlignment(Qt::AlignCenter);
  empty->setWordWrap(true);
  empty->setContentsMargins(24, 0, 24, 0);
  pages_->addWidget(empty);

  auto* page = new QWidget(pages_);
  player_ = new EvidencePlayer(client, thumbnails, page);
  auto* playerRule = new QWidget(page);
  playerRule->setObjectName(QStringLiteral("DetailRule"));
  playerRule->setAttribute(Qt::WA_StyledBackground, true);
  playerRule->setFixedHeight(1);

  auto* body = new QWidget();
  body->setObjectName(QStringLiteral("DetailBody"));
  evidenceChip_ = new ToneChip(kHeaderChipHeight, kHeaderChipPaddingX, body);
  evidenceReason_ = new QLabel(body);
  evidenceReason_->setObjectName(QStringLiteral("DetailSecondary"));
  evidenceReason_->setWordWrap(true);
  auto* evidenceRow = new QHBoxLayout();
  evidenceRow->setContentsMargins(0, 0, 0, 0);
  evidenceRow->setSpacing(8);
  evidenceRow->addWidget(evidenceChip_, 0, Qt::AlignTop);
  evidenceRow->addWidget(evidenceReason_, 1);

  title_ = new QLabel(body);
  title_->setObjectName(QStringLiteral("DetailTitle"));
  title_->setFont(Theme::subhead());
  title_->setWordWrap(true);
  detail_ = new QLabel(body);
  detail_->setObjectName(QStringLiteral("DetailSecondary"));
  detail_->setWordWrap(true);
  auto* summary = new QVBoxLayout();
  summary->setContentsMargins(0, 0, 0, 0);
  summary->setSpacing(4);
  summary->addWidget(title_);
  summary->addWidget(detail_);

  auto* meta = new QVBoxLayout();
  meta->setContentsMargins(0, 0, 0, 0);
  meta->setSpacing(kMetaGap);
  for (const char* key : kMetaKeys) {
    auto* row = new QWidget(body);
    row->setObjectName(QStringLiteral("MetaRow"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    auto* keyLabel = new QLabel(QString::fromLatin1(key), row);
    keyLabel->setObjectName(QStringLiteral("MetaKey"));
    auto* value = new QLabel(row);
    value->setObjectName(QStringLiteral("MetaValue"));
    value->setFont(Theme::mono(tk::font::small));
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, kMetaPaddingBottom);
    rowLayout->addWidget(keyLabel);
    rowLayout->addStretch(1);
    rowLayout->addWidget(value);
    meta->addWidget(row);
    metaValues_.push_back(value);
  }

  auto* activityLabel = new QLabel(QStringLiteral("ACTIVITY"), body);
  activityLabel->setProperty("role", QStringLiteral("section-label"));
  activityLabel->setFont(Theme::monoLabel());
  activity_ = new QLabel(body);
  activity_->setObjectName(QStringLiteral("DetailActivity"));
  activity_->setWordWrap(true);
  activity_->setTextFormat(Qt::RichText);
  auto* activityBlock = new QVBoxLayout();
  activityBlock->setContentsMargins(0, 0, 0, 0);
  activityBlock->setSpacing(6);
  activityBlock->addWidget(activityLabel);
  activityBlock->addWidget(activity_);

  auto* bodyColumn = new QVBoxLayout(body);
  bodyColumn->setContentsMargins(kBodyPadding, kBodyPadding, kBodyPadding, kBodyPadding);
  bodyColumn->setSpacing(kBodyGap);
  bodyColumn->addLayout(evidenceRow);
  bodyColumn->addLayout(summary);
  bodyColumn->addLayout(meta);
  bodyColumn->addLayout(activityBlock);
  bodyColumn->addStretch(1);
  auto* scroll = new QScrollArea(page);
  scroll->setObjectName(QStringLiteral("DetailScroll"));
  scroll->setWidget(body);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  auto* actions = new QWidget(page);
  const char* const reviewTexts[] = {"Confirmed", "False alarm", "Undecided"};
  auto* reviewRow = new QHBoxLayout();
  reviewRow->setContentsMargins(0, 0, 0, 0);
  reviewRow->setSpacing(8);
  for (size_t i = 0; i < review_.size(); ++i) {
    auto* b = new QPushButton(QString::fromLatin1(reviewTexts[i]), actions);
    b->setProperty("role", QStringLiteral("secondary"));
    b->setObjectName(QStringLiteral("ReviewButton"));
    b->setCheckable(true);
    Theme::setVariant(b, "lg");
    b->setCursor(Qt::PointingHandCursor);
    reviewRow->addWidget(b, 1);
    review_[i] = b;
    connect(b, &QPushButton::clicked, this, [this, i] {
      review_[i]->setChecked(shown_ && shown_->review && shown_->review->label == QLatin1StringView(kReviewLabels[i]));
      if (!eventId_.isEmpty()) store_.review(eventId_, QString::fromLatin1(kReviewLabels[i]));
    });
  }
  acknowledge_ = new QPushButton(QStringLiteral("Acknowledge"), actions);
  resolve_ = new QPushButton(QStringLiteral("Resolve"), actions);
  auto* stateRow = new QHBoxLayout();
  stateRow->setContentsMargins(0, 0, 0, 0);
  stateRow->setSpacing(8);
  for (QPushButton* b : {acknowledge_, resolve_}) {
    Theme::setVariant(b, "lg");
    b->setCursor(Qt::PointingHandCursor);
    stateRow->addWidget(b, 1);
  }
  error_ = new QLabel(actions);
  error_->setProperty("tone", QStringLiteral("critical"));
  error_->setObjectName(QStringLiteral("DetailError"));
  error_->setWordWrap(true);
  error_->hide();
  auto* actionsColumn = new QVBoxLayout(actions);
  actionsColumn->setContentsMargins(kBodyPadding, 12, kBodyPadding, kBodyPadding);
  actionsColumn->setSpacing(8);
  actionsColumn->addWidget(error_);
  actionsColumn->addLayout(reviewRow);
  actionsColumn->addLayout(stateRow);

  auto* pageColumn = new QVBoxLayout(page);
  pageColumn->setContentsMargins(0, 0, 0, 0);
  pageColumn->setSpacing(0);
  pageColumn->addWidget(player_);
  pageColumn->addWidget(playerRule);
  pageColumn->addWidget(scroll, 1);
  pageColumn->addWidget(actions);
  pages_->addWidget(page);

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(header);
  column->addWidget(pages_, 1);

  connect(acknowledge_, &QPushButton::clicked, this, [this] {
    if (!eventId_.isEmpty()) store_.acknowledge(eventId_);
  });
  connect(resolve_, &QPushButton::clicked, this, [this] {
    if (!eventId_.isEmpty()) store_.resolve(eventId_);
  });
  connect(&store_, &EventStore::eventsChanged, this, &AlertDetailPanel::fetch);
  connect(&store_, &EventStore::rulesChanged, this, [this] {
    if (shown_) apply(*shown_);
  });
  connect(&store_, &EventStore::eventUpdated, this, [this](const QString& id) {
    if (id == eventId_) error_->hide();
  });
  connect(&store_, &EventStore::actionFailed, this, [this](const QString& message) {
    error_->setText(message);
    error_->show();
  });
  setEventId(QString());
}

void AlertDetailPanel::setCameras(const QVector<fovea::Camera>& cameras) {
  cameras_ = cameras;
}

QString AlertDetailPanel::cameraLabel(const QString& cameraId) const {
  for (const fovea::Camera& c : cameras_)
    if (c.id == cameraId) return c.code.isEmpty() ? c.name : QStringLiteral("%1 · %2").arg(c.code, c.name);
  return QStringLiteral("unknown camera");
}

void AlertDetailPanel::setEventId(const QString& eventId) {
  if (eventId == eventId_ && shown_) return;
  eventId_ = eventId;
  shown_.reset();
  error_->hide();
  if (eventId_.isEmpty()) {
    severity_->set(QString(), Tone::Neutral);
    player_->setEvidence(QString(), std::nullopt);
    pages_->setCurrentIndex(0);
    return;
  }
  pages_->setCurrentIndex(1);
  if (const EventInfo* listed = store_.findEvent(eventId_)) apply(*listed);
  fetch();
}

// The list payload has no evaluations; the timeline comes from GET /v1/events/{id}.
void AlertDetailPanel::fetch() {
  if (eventId_.isEmpty()) return;
  const int seq = ++fetchSeq_;
  const QString id = eventId_;
  client_.getEvent(id, [this, seq, id](bool ok, const QJsonDocument& doc, const QString& error) {
    if (seq != fetchSeq_ || id != eventId_) return;
    if (!ok) {
      if (!shown_) {
        error_->setText(QStringLiteral("Could not load the alert · %1").arg(error));
        error_->show();
      }
      return;
    }
    apply(EventInfo::fromJson(doc.object()));
  }, this);
}

void AlertDetailPanel::setMeta(int row, const QString& value) {
  QLabel* label = metaValues_[row];
  label->setText(QFontMetrics(label->font()).elidedText(value, Qt::ElideMiddle, 200));
  label->setToolTip(value);
}

void AlertDetailPanel::apply(const EventInfo& event) {
  const bool hadTimeline = shown_ && shown_->id == event.id && !shown_->evaluations.isEmpty();
  EventInfo e = event;
  if (hadTimeline && e.evaluations.isEmpty()) e.evaluations = shown_->evaluations;
  shown_ = e;

  severity_->set(severityLabel(e.severity), severityTone(e.severity));
  player_->setEvidence(e.cameraId, e.evidence);
  if (e.evidence) {
    const QString state = e.evidence->state.isEmpty() ? QStringLiteral("unknown") : e.evidence->state;
    evidenceChip_->set(QStringLiteral("EVIDENCE %1").arg(state.toUpper()), evidenceTone(e.evidence->state));
    evidenceReason_->setText(evidenceReason(*e.evidence));
  } else {
    evidenceChip_->set(QStringLiteral("NO EVIDENCE"), Tone::Neutral);
    evidenceReason_->setText(QStringLiteral("the service recorded no evidence window"));
  }
  title_->setText(e.title);
  detail_->setText(e.detail);
  detail_->setVisible(!e.detail.isEmpty());

  const RuleInfo* rule = store_.findRule(e.ruleId);
  setMeta(MetaAlertId, e.id.left(8).toUpper());
  setMeta(MetaRule, rule ? QStringLiteral("%1 · rev %2").arg(rule->name).arg(e.ruleRevision) : store_.ruleLabel(e.ruleId));
  setMeta(MetaCamera, cameraLabel(e.cameraId));
  setMeta(MetaRaised, wallTime(e.openedUtcMs, QStringLiteral("yyyy-MM-dd HH:mm:ss")));
  setMeta(MetaCondition, e.clearedUtcMs > 0 ? QStringLiteral("%1 · %2").arg(e.condition, wallTime(e.clearedUtcMs, QStringLiteral("HH:mm:ss")))
                                        : e.condition);
  setMeta(MetaStatus, operatorStateLabel(e.operatorState));
  setMeta(MetaReview, e.review ? reviewLabel(e.review->label) : QStringLiteral("—"));
  setMeta(MetaEvidence, e.evidence ? QStringLiteral("%1–%2").arg(wallTime(e.evidence->fromUtcMs, QStringLiteral("HH:mm:ss")),
                                                              wallTime(e.evidence->toUtcMs, QStringLiteral("HH:mm:ss")))
                               : QStringLiteral("—"));

  QStringList lines;
  const QVector<ActivityLine> activity = activityLines(e);
  for (const ActivityLine& line : activity) {
    if (lines.size() >= kMaxActivityLines) break;
    lines << QStringLiteral("%1 — %2").arg(wallTime(line.utcMs, QStringLiteral("HH:mm:ss")), line.text.toHtmlEscaped());
  }
  activity_->setText(QStringLiteral("<div style=\"line-height:170%\">%1</div>").arg(lines.join(QStringLiteral("<br>"))));

  const QString label = e.review ? e.review->label : QString();
  for (size_t i = 0; i < review_.size(); ++i) review_[i]->setChecked(label == QLatin1StringView(kReviewLabels[i]));
  const bool isNew = e.operatorState == QLatin1StringView("new");
  const bool resolved = e.operatorState == QLatin1StringView("resolved");
  acknowledge_->setEnabled(isNew);
  resolve_->setEnabled(!resolved);
  acknowledge_->setProperty("role", isNew ? QStringLiteral("primary") : QStringLiteral("secondary"));
  resolve_->setProperty("role", !isNew && !resolved ? QStringLiteral("primary") : QStringLiteral("secondary"));
  restyle(acknowledge_);
  restyle(resolve_);
}

}

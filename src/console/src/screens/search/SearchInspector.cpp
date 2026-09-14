#include "screens/search/SearchInspector.h"
#include "core/AlertTypes.h"
#include "screens/alerts/EvidencePlayer.h"
#include "search/SearchLogic.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "util/Format.h"
#include <QDateTime>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kBodyPadding = 16;
constexpr int kBodyGap = 14;
constexpr int kMetaGap = 8;
constexpr int kMetaPaddingBottom = 7;
constexpr int kMetaValueWidth = 224;
constexpr int kNotePaddingX = 10;
constexpr int kNotePaddingY = 8;
constexpr int kActionGap = 8;
// One sample has no length of its own; its clip covers the default sampling interval.
constexpr int64_t kSampleClipMs = 1000;
const char* const kMetaKeys[] = {"Camera", "Start", "End", "Duration", "Samples", "Footage", "Index version", "Model"};
enum MetaRow { MetaCamera, MetaStart, MetaEnd, MetaDuration, MetaSamples, MetaFootage, MetaIndexVersion, MetaModel };

QString wallTime(int64_t utcMs, const QString& format) {
  return utcMs > 0 ? QDateTime::fromMSecsSinceEpoch(utcMs).toLocalTime().toString(format) : QStringLiteral("—");
}

QString footageLabel(const QString& state) {
  if (state == QLatin1StringView("partial")) return QStringLiteral("partly deleted");
  if (state == QLatin1StringView("deleted")) return QStringLiteral("deleted");
  if (state.isEmpty() || state == QLatin1StringView("available")) return QStringLiteral("recorded");
  return state;
}
}

SearchInspector::SearchInspector(CoreClient& client, ThumbnailCache& thumbnails, QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("SearchInspector"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedWidth(tk::size::inspector);

  auto* header = new QWidget(this);
  header->setObjectName(QStringLiteral("DetailHeader"));
  header->setAttribute(Qt::WA_StyledBackground, true);
  header->setFixedHeight(tk::size::panelHeader);
  auto* heading = new QLabel(QStringLiteral("SELECTED RESULT"), header);
  heading->setProperty("role", QStringLiteral("section-label"));
  heading->setFont(Theme::monoLabel());
  relevance_ = new QLabel(header);
  relevance_->setObjectName(QStringLiteral("InspectorRelevance"));
  relevance_->setFont(Theme::mono(tk::font::label));
  relevance_->setToolTip(QStringLiteral("Similarity score used for ranking; not a probability"));
  auto* headerRow = new QHBoxLayout(header);
  headerRow->setContentsMargins(kBodyPadding, 0, kBodyPadding, 0);
  headerRow->addWidget(heading);
  headerRow->addStretch(1);
  headerRow->addWidget(relevance_);

  pages_ = new QStackedWidget(this);
  auto* empty = new QLabel(QStringLiteral("Select a result to play its footage."), pages_);
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
  title_ = new QLabel(body);
  title_->setObjectName(QStringLiteral("DetailTitle"));
  title_->setFont(Theme::subhead());
  title_->setWordWrap(true);
  range_ = new QLabel(body);
  range_->setObjectName(QStringLiteral("DetailSecondary"));
  range_->setWordWrap(true);
  auto* summary = new QVBoxLayout();
  summary->setContentsMargins(0, 0, 0, 0);
  summary->setSpacing(4);
  summary->addWidget(title_);
  summary->addWidget(range_);

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

  auto* bodyColumn = new QVBoxLayout(body);
  bodyColumn->setContentsMargins(kBodyPadding, kBodyPadding, kBodyPadding, kBodyPadding);
  bodyColumn->setSpacing(kBodyGap);
  bodyColumn->addLayout(summary);
  bodyColumn->addLayout(meta);
  bodyColumn->addStretch(1);
  auto* scroll = new QScrollArea(page);
  scroll->setObjectName(QStringLiteral("DetailScroll"));
  scroll->setWidget(body);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  auto* pageColumn = new QVBoxLayout(page);
  pageColumn->setContentsMargins(0, 0, 0, 0);
  pageColumn->setSpacing(0);
  pageColumn->addWidget(player_);
  pageColumn->addWidget(playerRule);
  pageColumn->addWidget(scroll, 1);
  pages_->addWidget(page);

  auto* footer = new QWidget(this);
  auto* note = new QWidget(footer);
  note->setObjectName(QStringLiteral("SimilarityNote"));
  note->setAttribute(Qt::WA_StyledBackground, true);
  auto* noteText = new QLabel(QStringLiteral("Similarity search. Results are not verified."), note);
  noteText->setObjectName(QStringLiteral("SimilarityNoteText"));
  noteText->setWordWrap(true);
  auto* noteRow = new QHBoxLayout(note);
  noteRow->setContentsMargins(kNotePaddingX, kNotePaddingY, kNotePaddingX, kNotePaddingY);
  noteRow->addWidget(noteText);
  auto* actions = new QGridLayout();
  actions->setContentsMargins(0, 0, 0, 0);
  actions->setHorizontalSpacing(kActionGap);
  actions->setVerticalSpacing(kActionGap);
  const char* const actionTexts[] = {"Export clip", "Track subject", "Reference image", "Save as alert rule"};
  for (int i = 0; i < 4; ++i) {
    auto* button = new QPushButton(QString::fromLatin1(actionTexts[i]), footer);
    button->setProperty("role", QStringLiteral("secondary"));
    Theme::setVariant(button, "lg");
    button->setEnabled(false);
    button->setToolTip(QStringLiteral("Not implemented in this build"));
    actions->addWidget(button, i / 2, i % 2);
  }
  auto* notImplemented = new QLabel(QStringLiteral("not implemented"), footer);
  notImplemented->setProperty("role", QStringLiteral("hint"));
  notImplemented->setAlignment(Qt::AlignCenter);
  auto* footerColumn = new QVBoxLayout(footer);
  footerColumn->setContentsMargins(kBodyPadding, 0, kBodyPadding, kBodyPadding);
  footerColumn->setSpacing(kActionGap);
  footerColumn->addWidget(note);
  footerColumn->addLayout(actions);
  footerColumn->addWidget(notImplemented);

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(header);
  column->addWidget(pages_, 1);
  column->addWidget(footer);

  setResult(std::nullopt, QString(), QString());
}

void SearchInspector::setMeta(int row, const QString& value) {
  QLabel* label = metaValues_[row];
  label->setText(QFontMetrics(label->font()).elidedText(value, Qt::ElideMiddle, kMetaValueWidth));
  label->setToolTip(value);
}

void SearchInspector::setResult(const std::optional<SearchResultRow>& row, const QString& indexVersion,
                                const QString& model) {
  if (!row) {
    relevance_->clear();
    player_->setEvidence(QString(), std::nullopt);
    pages_->setCurrentIndex(0);
    return;
  }
  const SearchResultInfo& r = row->result;
  relevance_->setText(relevanceLabel(r.relevance));
  EvidenceInfo clip;
  clip.id = r.recordId;
  clip.state = r.evidenceState.isEmpty() ? QStringLiteral("available") : r.evidenceState;
  clip.fromUtcMs = r.startUtcMs;
  clip.toUtcMs = std::max(r.endUtcMs, r.startUtcMs + kSampleClipMs);
  player_->setEvidence(r.cameraId, clip);

  title_->setText(row->cameraName);
  const int64_t lengthMs = r.endUtcMs - r.startUtcMs;
  range_->setText(QStringLiteral("%1 – %2 · %3").arg(wallTime(r.startUtcMs, QStringLiteral("HH:mm:ss")),
                                                     wallTime(r.endUtcMs, QStringLiteral("HH:mm:ss")),
                                                     lengthMs > 0 ? durationLabel(lengthMs) : QStringLiteral("one sampled frame")));
  const QString dateTime = QStringLiteral("yyyy-MM-dd HH:mm:ss");
  setMeta(MetaCamera, row->cameraCode.isEmpty() ? row->cameraName : QStringLiteral("%1 · %2").arg(row->cameraCode, row->cameraName));
  setMeta(MetaStart, wallTime(r.startUtcMs, dateTime));
  setMeta(MetaEnd, wallTime(r.endUtcMs, dateTime));
  setMeta(MetaDuration, durationLabel(lengthMs));
  setMeta(MetaSamples, r.samples > 0 ? QString::number(r.samples) : QStringLiteral("—"));
  setMeta(MetaFootage, footageLabel(r.evidenceState));
  setMeta(MetaIndexVersion, indexVersion.isEmpty() ? QStringLiteral("—") : indexVersion);
  setMeta(MetaModel, model.isEmpty() ? QStringLiteral("—") : model);
  pages_->setCurrentIndex(1);
}

void SearchInspector::togglePlayback() {
  if (pages_->currentIndex() == 1) player_->togglePlayback();
}

}

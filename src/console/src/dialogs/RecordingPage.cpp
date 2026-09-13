#include "dialogs/RecordingPage.h"
#include "widgets/FormField.h"
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

namespace fovea::ui {

namespace {
constexpr int kPagePadding = 20;
constexpr int kRowGap = 18;
constexpr int kSegmentMin = 10;
constexpr int kSegmentMax = 600;
constexpr int kSegmentDefault = 60;
constexpr int kFieldWidth = 180;
}

RecordingPage::RecordingPage(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("RecordingPage"));
  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(kPagePadding, kPagePadding, kPagePadding, kPagePadding);
  column->setSpacing(kRowGap);
  segment_ = new QSpinBox(this);
  segment_->setObjectName(QStringLiteral("SegmentSeconds"));
  segment_->setRange(kSegmentMin, kSegmentMax);
  segment_->setSingleStep(10);
  segment_->setValue(kSegmentDefault);
  segment_->setSuffix(QStringLiteral(" s"));
  segment_->setFixedWidth(kFieldWidth);
  column->addWidget(formField(QStringLiteral("Segment length"), segment_, this,
                              QStringLiteral("— each recording file covers this many seconds")), 0, Qt::AlignLeft);
  auto* note = hintLabel(QStringLiteral("Provisional tab: retention and storage location arrive with M2 and have no design yet."), this);
  note->setWordWrap(true);
  column->addWidget(note);
  column->addStretch(1);
}

void RecordingPage::setSegmentSeconds(int seconds) {
  segment_->setValue(std::clamp(seconds, kSegmentMin, kSegmentMax));
}

int RecordingPage::segmentSeconds() const { return segment_->value(); }

}

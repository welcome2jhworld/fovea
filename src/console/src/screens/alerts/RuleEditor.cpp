#include "screens/alerts/RuleEditor.h"
#include "core/CoreClient.h"
#include "core/EventStore.h"
#include "screens/alerts/ZoneCanvas.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "widgets/FormField.h"
#include "widgets/ToggleSwitch.h"
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QTimeEdit>
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kSectionGap = 12;
constexpr int kLabelGap = 6;
constexpr int kGridGap = 10;
constexpr int kActionGap = 9;
constexpr int kDayHeight = 24;
constexpr int kMinutesPerDay = 24 * 60;
const char* const kDayLabels[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
// The classes the rule API accepts and the worker detects.
const char* const kClasses[] = {"person", "car", "truck", "bus", "motorcycle", "bicycle"};

QLabel* editorLabel(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", QStringLiteral("editor-label"));
  return label;
}

QWidget* editorField(const QString& label, QWidget* control, QWidget* parent, QWidget* action = nullptr) {
  auto* field = new QWidget(parent);
  auto* column = new QVBoxLayout(field);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(kLabelGap);
  auto* labelRow = new QHBoxLayout();
  labelRow->setContentsMargins(0, 0, 0, 0);
  labelRow->addWidget(editorLabel(label, field));
  labelRow->addStretch(1);
  if (action) {
    action->setParent(field);
    labelRow->addWidget(action);
  }
  column->addLayout(labelRow);
  control->setParent(field);
  column->addWidget(control);
  return field;
}

QTime timeOfMinute(int minute) {
  const int m = minute % kMinutesPerDay;
  return QTime(m / 60, m % 60);
}

int minuteOfTime(const QTime& t) { return t.hour() * 60 + t.minute(); }

QJsonArray pointsJson(const QVector<QPointF>& points) {
  QJsonArray out;
  for (const QPointF& p : points)
    out.push_back(QJsonArray{std::round(p.x() * 10000.0) / 10000.0, std::round(p.y() * 10000.0) / 10000.0});
  return out;
}

QWidget* actionRow(const QString& text, ToggleSwitch* toggle, QWidget* parent, const QString& hint = {}) {
  auto* row = new QWidget(parent);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  auto* label = new QLabel(text, row);
  layout->addWidget(label);
  if (!hint.isEmpty()) layout->addWidget(hintLabel(hint, row));
  layout->addStretch(1);
  toggle->setParent(row);
  layout->addWidget(toggle);
  return row;
}
}

RuleEditor::RuleEditor(CoreClient& client, EventStore& store, QWidget* parent)
    : QWidget(parent), client_(client), store_(store) {
  setObjectName(QStringLiteral("RuleEditor"));

  name_ = new QLineEdit(this);
  name_->setObjectName(QStringLiteral("RuleName"));
  name_->setPlaceholderText(QStringLiteral("Rule name"));
  camera_ = new QComboBox(this);
  camera_->setObjectName(QStringLiteral("RuleCamera"));
  zone_ = new ZoneCanvas(this);
  auto* clearZone = new QPushButton(QStringLiteral("Clear"));
  clearZone->setProperty("role", QStringLiteral("link"));
  clearZone->setCursor(Qt::PointingHandCursor);
  clearZone->setFocusPolicy(Qt::NoFocus);
  zoneHint_ = hintLabel(QString(), this);
  zoneHint_->setWordWrap(true);

  auto* daysRow = new QWidget(this);
  auto* daysLayout = new QHBoxLayout(daysRow);
  daysLayout->setContentsMargins(0, 0, 0, 0);
  daysLayout->setSpacing(4);
  for (const char* day : kDayLabels) {
    auto* b = new QToolButton(daysRow);
    b->setText(QString::fromLatin1(day));
    b->setProperty("role", QStringLiteral("day"));
    b->setCheckable(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(kDayHeight);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setFont(Theme::mono(tk::font::label));
    daysLayout->addWidget(b);
    days_.push_back(b);
  }
  start_ = new QTimeEdit(this);
  end_ = new QTimeEdit(this);
  for (QTimeEdit* t : {start_, end_}) {
    t->setDisplayFormat(QStringLiteral("HH:mm"));
    t->setButtonSymbols(QAbstractSpinBox::NoButtons);
  }
  timeZone_ = new QComboBox(this);
  for (const QByteArray& id : QTimeZone::availableTimeZoneIds()) timeZone_->addItem(QString::fromUtf8(id));

  targetClass_ = new QComboBox(this);
  for (const char* cls : kClasses) targetClass_->addItem(QString::fromLatin1(cls));
  confidence_ = new QDoubleSpinBox(this);
  confidence_->setObjectName(QStringLiteral("RuleConfidence"));
  confidence_->setDecimals(1);
  confidence_->setRange(0.0, 100.0);
  confidence_->setSingleStep(5.0);
  confidence_->setPrefix(QStringLiteral("≥ "));
  confidence_->setSuffix(QStringLiteral("%"));
  dwell_ = new QDoubleSpinBox(this);
  dwell_->setObjectName(QStringLiteral("RuleDwell"));
  dwell_->setDecimals(1);
  dwell_->setRange(1.0, 3600.0);
  dwell_->setSingleStep(0.1);
  dwell_->setSuffix(QStringLiteral(" s"));
  severity_ = new QComboBox(this);
  severity_->addItem(QStringLiteral("Critical"), QStringLiteral("critical"));
  severity_->addItem(QStringLiteral("Review"), QStringLiteral("review"));
  severity_->addItem(QStringLiteral("Info"), QStringLiteral("info"));
  for (QWidget* w : std::initializer_list<QWidget*>{name_, camera_, start_, end_, timeZone_, targetClass_, confidence_,
                                                    dwell_, severity_})
    w->setProperty("surface", QStringLiteral("app"));
  for (QAbstractSpinBox* box : std::initializer_list<QAbstractSpinBox*>{confidence_, dwell_})
    box->setButtonSymbols(QAbstractSpinBox::NoButtons);

  sound_ = new ToggleSwitch(ToggleSwitch::Size::Sm);
  pop_ = new ToggleSwitch(ToggleSwitch::Size::Sm);
  auto* webhook = new ToggleSwitch(ToggleSwitch::Size::Sm);
  auto* email = new ToggleSwitch(ToggleSwitch::Size::Sm);
  webhook->setEnabled(false);
  email->setEnabled(false);

  auto* scheduleTimes = new QWidget(this);
  auto* timesGrid = new QGridLayout(scheduleTimes);
  timesGrid->setContentsMargins(0, 0, 0, 0);
  timesGrid->setHorizontalSpacing(kGridGap);
  timesGrid->setVerticalSpacing(kGridGap);
  timesGrid->addWidget(editorField(QStringLiteral("Start"), start_, scheduleTimes), 0, 0);
  timesGrid->addWidget(editorField(QStringLiteral("End"), end_, scheduleTimes), 0, 1);
  timesGrid->addWidget(editorField(QStringLiteral("Time zone"), timeZone_, scheduleTimes), 1, 0, 1, 2);

  auto* thresholds = new QWidget(this);
  auto* thresholdGrid = new QGridLayout(thresholds);
  thresholdGrid->setContentsMargins(0, 0, 0, 0);
  thresholdGrid->setHorizontalSpacing(kGridGap);
  thresholdGrid->setVerticalSpacing(kSectionGap);
  thresholdGrid->addWidget(editorField(QStringLiteral("Target class"), targetClass_, thresholds), 0, 0);
  thresholdGrid->addWidget(editorField(QStringLiteral("Confidence"), confidence_, thresholds), 0, 1);
  thresholdGrid->addWidget(editorField(QStringLiteral("Dwell"), dwell_, thresholds), 1, 0);
  thresholdGrid->addWidget(editorField(QStringLiteral("Severity"), severity_, thresholds), 1, 1);

  auto* actions = new QWidget(this);
  auto* actionsColumn = new QVBoxLayout(actions);
  actionsColumn->setContentsMargins(0, 0, 0, 0);
  actionsColumn->setSpacing(kActionGap);
  actionsColumn->addWidget(editorLabel(QStringLiteral("Actions"), actions));
  actionsColumn->addWidget(actionRow(QStringLiteral("Sound alarm on wall"), sound_, actions));
  actionsColumn->addWidget(actionRow(QStringLiteral("Pop camera to main view"), pop_, actions));
  actionsColumn->addWidget(actionRow(QStringLiteral("Webhook to SOC bridge"), webhook, actions, QStringLiteral("not implemented")));
  actionsColumn->addWidget(actionRow(QStringLiteral("Email duty officer"), email, actions, QStringLiteral("not implemented")));

  error_ = new QLabel(this);
  error_->setProperty("tone", QStringLiteral("critical"));
  error_->setObjectName(QStringLiteral("RuleEditorError"));
  error_->setWordWrap(true);
  error_->hide();

  auto* test = new QPushButton(QStringLiteral("Test on last 24 h"), this);
  test->setProperty("role", QStringLiteral("secondary"));
  test->setEnabled(false);
  test->setToolTip(QStringLiteral("Replaying a rule against stored footage is not implemented"));
  auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
  cancel->setProperty("role", QStringLiteral("ghost"));
  saveButton_ = new QPushButton(QStringLiteral("Save"), this);
  saveButton_->setObjectName(QStringLiteral("RuleSave"));
  saveButton_->setProperty("role", QStringLiteral("primary"));
  for (QPushButton* b : {test, cancel, saveButton_}) {
    Theme::setVariant(b, "lg");
    b->setCursor(Qt::PointingHandCursor);
  }
  test->setObjectName(QStringLiteral("RuleTest"));
  cancel->setObjectName(QStringLiteral("RuleCancel"));
  auto* buttons = new QHBoxLayout();
  buttons->setContentsMargins(0, 0, 0, 0);
  buttons->setSpacing(8);
  buttons->addWidget(test);
  buttons->addStretch(1);
  buttons->addWidget(cancel);
  buttons->addWidget(saveButton_);

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 2, 0, 0);
  column->setSpacing(kSectionGap);
  column->addWidget(editorField(QStringLiteral("Name"), name_, this));
  column->addWidget(editorField(QStringLiteral("Camera"), camera_, this));
  auto* zoneBlock = new QWidget(this);
  auto* zoneColumn = new QVBoxLayout(zoneBlock);
  zoneColumn->setContentsMargins(0, 0, 0, 0);
  zoneColumn->setSpacing(kLabelGap);
  zoneColumn->addWidget(editorField(QStringLiteral("Zone"), zone_, zoneBlock, clearZone));
  zoneColumn->addWidget(zoneHint_);
  column->addWidget(zoneBlock);
  auto* scheduleBlock = new QWidget(this);
  auto* scheduleColumn = new QVBoxLayout(scheduleBlock);
  scheduleColumn->setContentsMargins(0, 0, 0, 0);
  scheduleColumn->setSpacing(kLabelGap);
  scheduleColumn->addWidget(editorLabel(QStringLiteral("Schedule"), scheduleBlock));
  scheduleColumn->addWidget(daysRow);
  scheduleColumn->addWidget(scheduleTimes);
  column->addWidget(scheduleBlock);
  column->addWidget(thresholds);
  column->addWidget(actions);
  column->addWidget(error_);
  column->addLayout(buttons);
  column->addWidget(hintLabel(QStringLiteral("Test on last 24 h is not implemented."), this));

  connect(camera_, &QComboBox::currentIndexChanged, this, &RuleEditor::onCameraChanged);
  connect(zone_, &ZoneCanvas::changed, this, &RuleEditor::updateZoneHint);
  connect(clearZone, &QPushButton::clicked, zone_, &ZoneCanvas::clearPoints);
  connect(severity_, &QComboBox::currentIndexChanged, this, &RuleEditor::updateSeverityTone);
  connect(cancel, &QPushButton::clicked, this, &RuleEditor::cancelled);
  connect(saveButton_, &QPushButton::clicked, this, &RuleEditor::save);
  load(std::nullopt);
}

void RuleEditor::setCameras(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses) {
  cameras_ = cameras;
  statuses_ = statuses;
  QStringList items;
  for (const fovea::Camera& c : cameras_) {
    const QString label = c.code.isEmpty() ? c.name : QStringLiteral("%1 · %2").arg(c.code, c.name);
    items << (c.analyticsEnabled ? label : QStringLiteral("%1 (analytics off)").arg(label)) << c.id;
  }
  if (items != cameraItems_) {
    cameraItems_ = items;
    const QString current = camera_->currentData().toString();
    const QString wanted = current.isEmpty() && base_ ? base_->cameraId : current;
    const QSignalBlocker block(camera_);
    camera_->clear();
    for (qsizetype i = 0; i + 1 < items.size(); i += 2) camera_->addItem(items[i], items[i + 1]);
    const int index = camera_->findData(wanted);
    camera_->setCurrentIndex(index >= 0 ? index : 0);
  }
  onCameraChanged();
}

void RuleEditor::load(const std::optional<RuleInfo>& rule) {
  base_ = rule;
  const RuleInfo r = rule.value_or(RuleInfo{});
  loadedZone_.reset();
  if (rule) {
    if (const ZoneInfo* z = store_.findZone(rule->zoneId)) loadedZone_ = *z;
  }
  zoneCameraId_.clear();
  name_->setText(r.name);
  const ScheduleWindow window = r.schedule.isEmpty() ? ScheduleWindow{} : r.schedule.first();
  for (int i = 0; i < days_.size(); ++i) days_[i]->setChecked(window.days & (1 << i));
  start_->setTime(timeOfMinute(window.startMinute));
  end_->setTime(timeOfMinute(window.endMinute));
  const QString tz = r.timeZone.isEmpty() ? QString::fromUtf8(QTimeZone::systemTimeZoneId()) : r.timeZone;
  if (timeZone_->findText(tz) < 0) timeZone_->addItem(tz);
  timeZone_->setCurrentText(tz);
  targetClass_->setCurrentText(r.targetClass);
  confidence_->setValue(r.minConfidence * 100.0);
  dwell_->setValue(static_cast<double>(r.dwellNs) / static_cast<double>(RuleInfo::kNsPerSecond));
  loadedConfidence_ = confidence_->value();
  loadedDwell_ = dwell_->value();
  camera_->setEnabled(!rule.has_value());
  camera_->setToolTip(rule ? QStringLiteral("A rule stays on its camera; create a new rule for another camera") : QString());
  severity_->setCurrentIndex(std::max(0, severity_->findData(r.severity)));
  sound_->setChecked(rule ? r.soundAction : true);
  pop_->setChecked(rule ? r.popAction : true);
  error_->hide();
  saving_ = false;
  saveButton_->setEnabled(true);
  {
    const QSignalBlocker block(camera_);
    const int index = camera_->findData(r.cameraId);
    if (index >= 0) camera_->setCurrentIndex(index);
  }
  onCameraChanged();
  updateSeverityTone();
}

// A zone belongs to one camera: switching away shows an empty zone, switching back restores it.
void RuleEditor::onCameraChanged() {
  const QString cameraId = camera_->currentData().toString();
  std::optional<fovea::RingRef> ring;
  QString session;
  for (const fovea::CameraStatus& s : std::as_const(statuses_)) {
    if (s.cameraId != cameraId) continue;
    ring = s.frameRing;
    session = s.sessionId;
  }
  zone_->bind(ring, session);
  if (cameraId != zoneCameraId_) {
    zoneCameraId_ = cameraId;
    if (loadedZone_ && loadedZone_->cameraId == cameraId)
      zone_->setZone(loadedZone_->points, QSize(loadedZone_->refWidth, loadedZone_->refHeight));
    else
      zone_->setZone({}, {});
  }
  updateZoneHint();
}

void RuleEditor::updateZoneHint() {
  const int count = static_cast<int>(zone_->points().size());
  const QSize ref = zone_->referenceSize();
  QString text;
  if (!zone_->hasFrame()) {
    text = QStringLiteral("The zone is drawn on the camera's live frame; waiting for one.");
  } else if (count == 0) {
    text = QStringLiteral("Click the frame to add points; drag to move, right-click to remove.");
  } else {
    text = QStringLiteral("%1 point%2 · reference %3×%4").arg(count).arg(count == 1 ? QString() : QStringLiteral("s"))
               .arg(ref.width()).arg(ref.height());
    if (loadedZone_ && loadedZone_->refWidth > 0 &&
        (loadedZone_->refWidth != ref.width() || loadedZone_->refHeight != ref.height())) {
      text += QStringLiteral(" · drawn at %1×%2, saving updates it").arg(loadedZone_->refWidth).arg(loadedZone_->refHeight);
    }
  }
  zoneHint_->setText(text);
}

void RuleEditor::updateSeverityTone() {
  severity_->setProperty("tone", severity_->currentData().toString());
  restyle(severity_);
}

int RuleEditor::selectedDays() const {
  int mask = 0;
  for (int i = 0; i < days_.size(); ++i)
    if (days_[i]->isChecked()) mask |= 1 << i;
  return mask;
}

void RuleEditor::fail(const QString& message) {
  saving_ = false;
  saveButton_->setEnabled(true);
  error_->setText(message);
  error_->setVisible(!message.isEmpty());
}

void RuleEditor::save() {
  if (saving_) return;
  const QString name = name_->text().trimmed();
  const QString cameraId = camera_->currentData().toString();
  const QVector<QPointF> points = zone_->points();
  const QSize ref = zone_->referenceSize();
  const int days = selectedDays();
  if (name.isEmpty()) return fail(QStringLiteral("Name the rule."));
  if (cameraId.isEmpty()) return fail(QStringLiteral("Choose a camera."));
  if (points.size() < 3) return fail(QStringLiteral("Draw a zone with at least three points."));
  if (ref.isEmpty()) return fail(QStringLiteral("The camera has not shown a frame, so the zone has no reference size."));
  if (days == 0) return fail(QStringLiteral("Pick at least one day."));
  if (base_ && !base_->id.isEmpty() && cameraId != base_->cameraId)
    return fail(QStringLiteral("A rule cannot move to another camera; create a new rule there."));

  RuleInfo rule = base_.value_or(RuleInfo{});
  rule.name = name;
  rule.cameraId = cameraId;
  rule.enabled = enabledSource_ ? enabledSource_() : rule.enabled;
  int startMinute = minuteOfTime(start_->time());
  int endMinute = minuteOfTime(end_->time());
  if (endMinute == 0) endMinute = kMinutesPerDay;
  if (rule.schedule.isEmpty()) rule.schedule.push_back(ScheduleWindow{});
  rule.schedule.first() = ScheduleWindow{days, startMinute, endMinute};
  rule.timeZone = timeZone_->currentText();
  rule.targetClass = targetClass_->currentText();
  if (!base_ || confidence_->value() != loadedConfidence_) rule.minConfidence = confidence_->value() / 100.0;
  if (!base_ || dwell_->value() != loadedDwell_)
    rule.dwellNs = std::llround(dwell_->value() * static_cast<double>(RuleInfo::kNsPerSecond));
  rule.severity = severity_->currentData().toString();
  rule.soundAction = sound_->isChecked();
  rule.popAction = pop_->isChecked();

  saving_ = true;
  saveButton_->setEnabled(false);
  error_->hide();

  const bool sameZone = loadedZone_ && loadedZone_->cameraId == cameraId && loadedZone_->points == points &&
                        loadedZone_->refWidth == ref.width() && loadedZone_->refHeight == ref.height();
  if (sameZone) {
    rule.zoneId = loadedZone_->id;
    rule.zoneRevision = loadedZone_->revision;
    saveRule(rule);
    return;
  }
  const QString anchor = loadedZone_ ? loadedZone_->anchor : QStringLiteral("foot");
  QJsonObject zone{{"points", pointsJson(points)}, {"anchor", anchor}, {"ref_width", ref.width()},
                   {"ref_height", ref.height()}};
  auto onZone = [this, rule](bool ok, const QJsonDocument& doc, const QString& error) mutable {
    if (!ok) return fail(QStringLiteral("Saving the zone failed · %1").arg(error));
    const ZoneInfo saved = ZoneInfo::fromJson(doc.object());
    if (saved.id.isEmpty()) return fail(QStringLiteral("Saving the zone failed · the service returned no zone id"));
    loadedZone_ = saved;
    rule.zoneId = saved.id;
    rule.zoneRevision = saved.revision;
    saveRule(rule);
  };
  if (loadedZone_ && loadedZone_->cameraId == cameraId) {
    client_.updateZone(loadedZone_->id, zone, onZone, this);
  } else {
    zone.insert("camera_id", cameraId);
    zone.insert("name", name);
    client_.createZone(zone, onZone, this);
  }
}

void RuleEditor::saveRule(RuleInfo rule) {
  auto onRule = [this](bool ok, const QJsonDocument& doc, const QString& error) {
    if (!ok) return fail(QStringLiteral("Saving the rule failed · %1").arg(error));
    const RuleInfo saved = RuleInfo::fromJson(doc.object());
    saving_ = false;
    saveButton_->setEnabled(true);
    store_.refreshRules();
    emit this->saved(saved.id);
  };
  if (rule.id.isEmpty()) client_.createRule(rule.revisionJson(), onRule, this);
  else client_.updateRule(rule.id, rule.revisionJson(), onRule, this);
}

}

#include "dialogs/ConnectionPage.h"
#include "core/CoreClient.h"
#include "dialogs/FfmpegCommand.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include "util/Format.h"
#include "video/FrameSurface.h"
#include "widgets/FormField.h"
#include "widgets/StatusChip.h"
#include "widgets/ToggleSwitch.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr int kPagePadding = 20;
constexpr int kRowGap = 18;
constexpr int kGridGap = 14;
constexpr int kProtocolWidth = 180;
constexpr int kCodeWidth = 140;
constexpr int kTransportWidth = 140;
constexpr int kTimeoutWidth = 120;
constexpr int kJitterWidth = 120;
constexpr int kTestFrameWidth = 200;
constexpr int kTestFrameHeight = 118;
constexpr int kTestPadding = 14;
constexpr int kCopyNoteMs = 2500;
const QString kDash = QStringLiteral("—");

QLabel* metricKey(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  l->setProperty("role", QStringLiteral("metric-key"));
  return l;
}

QLabel* metricValue(QWidget* parent) {
  auto* l = new QLabel(kDash, parent);
  l->setProperty("role", QStringLiteral("metric-value"));
  l->setFont(Theme::mono(tk::font::small));
  return l;
}

QLineEdit* lineEdit(const char* name, QWidget* parent, bool mono = false) {
  auto* e = new QLineEdit(parent);
  e->setObjectName(QLatin1StringView(name));
  if (mono) e->setProperty("mono", true);
  return e;
}
}

ConnectionPage::ConnectionPage(CoreClient& client, QWidget* parent) : QWidget(parent), client_(client) {
  setObjectName(QStringLiteral("ConnectionPage"));
  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(kPagePadding, kPagePadding, kPagePadding, kPagePadding);
  column->setSpacing(kRowGap);
  buildForm();
  buildTestBlock();
  buildToggles();
  column->addStretch(1);
  applyProtocol();
  setTestState(TestState::Idle, {});
}

void ConnectionPage::buildForm() {
  auto* column = static_cast<QVBoxLayout*>(layout());

  name_ = lineEdit("DisplayName", this);
  name_->setPlaceholderText(QStringLiteral("Loading Dock B"));
  group_ = new QComboBox(this);
  group_->setObjectName(QStringLiteral("GroupSelect"));
  group_->setEditable(true);
  group_->setInsertPolicy(QComboBox::NoInsert);
  group_->lineEdit()->setPlaceholderText(QStringLiteral("No group"));
  code_ = lineEdit("CameraCode", this, true);
  code_->setFixedWidth(kCodeWidth);
  auto* row1 = new QGridLayout();
  row1->setContentsMargins(0, 0, 0, 0);
  row1->setHorizontalSpacing(kGridGap);
  row1->addWidget(formField(QStringLiteral("Display name"), name_, this), 0, 0);
  row1->addWidget(formField(QStringLiteral("Group"), group_, this), 0, 1);
  row1->addWidget(formField(QStringLiteral("Code"), code_, this), 0, 2);
  row1->setColumnStretch(0, 1);
  row1->setColumnStretch(1, 1);
  column->addLayout(row1);

  protocol_ = new QComboBox(this);
  protocol_->setObjectName(QStringLiteral("ProtocolSelect"));
  protocol_->addItem(QStringLiteral("RTSP"), QStringLiteral("rtsp"));
  protocol_->addItem(QStringLiteral("File"), QStringLiteral("file"));
  protocol_->setFixedWidth(kProtocolWidth);
  mainUrl_ = lineEdit("MainUrl", this, true);
  browse_ = new QPushButton(QStringLiteral("Browse…"), this);
  browse_->setObjectName(QStringLiteral("BrowseFile"));
  browse_->setProperty("role", QStringLiteral("link"));
  browse_->setCursor(Qt::PointingHandCursor);
  browse_->setFocusPolicy(Qt::NoFocus);
  QWidget* mainField = formField(QStringLiteral("Main stream URL"), mainUrl_, this, {}, browse_);
  mainUrlLabel_ = mainField->findChild<QLabel*>();
  auto* row2 = new QGridLayout();
  row2->setContentsMargins(0, 0, 0, 0);
  row2->setHorizontalSpacing(kGridGap);
  row2->addWidget(formField(QStringLiteral("Protocol"), protocol_, this), 0, 0);
  row2->addWidget(mainField, 0, 1);
  row2->setColumnStretch(1, 1);
  column->addLayout(row2);

  subUrl_ = lineEdit("SubUrl", this, true);
  subUrl_->setPlaceholderText(QStringLiteral("optional · falls back to the main stream"));
  jitter_ = new QSpinBox(this);
  jitter_->setObjectName(QStringLiteral("JitterBuffer"));
  jitter_->setRange(0, 10000);
  jitter_->setSingleStep(100);
  jitter_->setSuffix(QStringLiteral(" ms"));
  jitter_->setFixedWidth(kJitterWidth);
  auto* row3 = new QGridLayout();
  row3->setContentsMargins(0, 0, 0, 0);
  row3->setHorizontalSpacing(kGridGap);
  row3->addWidget(formField(QStringLiteral("Sub stream URL"), subUrl_, this, QStringLiteral("— used for the wall grid")), 0, 0);
  row3->addWidget(formField(QStringLiteral("Jitter buffer"), jitter_, this), 0, 1);
  row3->setColumnStretch(0, 1);
  column->addLayout(row3);

  username_ = lineEdit("Username", this);
  passwordField_ = new QFrame(this);
  passwordField_->setObjectName(QStringLiteral("PasswordField"));
  passwordField_->setAttribute(Qt::WA_StyledBackground, true);
  password_ = lineEdit("Password", passwordField_, true);
  password_->setEchoMode(QLineEdit::Password);
  password_->installEventFilter(this);
  reveal_ = new QToolButton(passwordField_);
  reveal_->setObjectName(QStringLiteral("RevealPassword"));
  reveal_->setText(QStringLiteral("show"));
  reveal_->setFont(Theme::mono(tk::font::label));
  reveal_->setCursor(Qt::PointingHandCursor);
  reveal_->setFocusPolicy(Qt::NoFocus);
  reveal_->setCheckable(true);
  auto* passwordRow = new QHBoxLayout(passwordField_);
  passwordRow->setContentsMargins(0, 0, 8, 0);
  passwordRow->setSpacing(4);
  passwordRow->addWidget(password_, 1);
  passwordRow->addWidget(reveal_);
  connect(reveal_, &QToolButton::toggled, this, [this](bool on) {
    password_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    reveal_->setText(on ? QStringLiteral("hide") : QStringLiteral("show"));
  });
  transport_ = new QComboBox(this);
  transport_->setObjectName(QStringLiteral("TransportSelect"));
  transport_->addItem(QStringLiteral("TCP"), QStringLiteral("tcp"));
  transport_->addItem(QStringLiteral("UDP"), QStringLiteral("udp"));
  transport_->setFixedWidth(kTransportWidth);
  timeout_ = new QSpinBox(this);
  timeout_->setObjectName(QStringLiteral("Timeout"));
  timeout_->setRange(1, 120);
  timeout_->setSuffix(QStringLiteral(" s"));
  timeout_->setFixedWidth(kTimeoutWidth);
  auto* row4 = new QGridLayout();
  row4->setContentsMargins(0, 0, 0, 0);
  row4->setHorizontalSpacing(kGridGap);
  row4->addWidget(formField(QStringLiteral("Username"), username_, this), 0, 0);
  row4->addWidget(formField(QStringLiteral("Password"), passwordField_, this), 0, 1);
  row4->addWidget(formField(QStringLiteral("Transport"), transport_, this), 0, 2);
  row4->addWidget(formField(QStringLiteral("Timeout"), timeout_, this), 0, 3);
  row4->setColumnStretch(0, 1);
  row4->setColumnStretch(1, 1);
  column->addLayout(row4);

  connect(protocol_, &QComboBox::currentIndexChanged, this, &ConnectionPage::applyProtocol);
  connect(browse_, &QPushButton::clicked, this, &ConnectionPage::browseFile);
}

void ConnectionPage::buildTestBlock() {
  auto* column = static_cast<QVBoxLayout*>(layout());
  auto* block = new QFrame(this);
  block->setObjectName(QStringLiteral("TestBlock"));
  block->setProperty("role", QStringLiteral("card"));
  block->setAttribute(Qt::WA_StyledBackground, true);
  auto* row = new QHBoxLayout(block);
  row->setContentsMargins(kTestPadding, kTestPadding, kTestPadding, kTestPadding);
  row->setSpacing(16);

  auto* frameBox = new QFrame(block);
  frameBox->setObjectName(QStringLiteral("TestFrame"));
  frameBox->setAttribute(Qt::WA_StyledBackground, true);
  frameBox->setFixedSize(kTestFrameWidth, kTestFrameHeight);
  testFrame_ = new FrameSurface(frameBox);
  testFrame_->setCornerRadius(tk::radius::input - 1);
  testFrame_->setCaption(QStringLiteral("NO PREVIEW"));
  auto* frameLayout = new QVBoxLayout(frameBox);
  frameLayout->setContentsMargins(1, 1, 1, 1);
  frameLayout->addWidget(testFrame_);
  row->addWidget(frameBox, 0, Qt::AlignTop);

  auto* right = new QVBoxLayout();
  right->setContentsMargins(0, 0, 0, 0);
  right->setSpacing(10);
  auto* statusRow = new QHBoxLayout();
  statusRow->setContentsMargins(0, 0, 0, 0);
  statusRow->setSpacing(10);
  testChip_ = new StatusChip(block);
  handshake_ = new QLabel(block);
  handshake_->setFont(Theme::mono(tk::font::label));
  handshake_->setProperty("tone", QStringLiteral("muted"));
  statusRow->addWidget(testChip_);
  statusRow->addWidget(handshake_);
  statusRow->addStretch(1);
  right->addLayout(statusRow);

  auto* metrics = new QGridLayout();
  metrics->setContentsMargins(0, 0, 0, 0);
  metrics->setHorizontalSpacing(20);
  metrics->setVerticalSpacing(6);
  resolution_ = metricValue(block);
  codec_ = metricValue(block);
  frameRate_ = metricValue(block);
  bitrate_ = metricValue(block);
  metrics->addWidget(metricKey(QStringLiteral("Resolution"), block), 0, 0);
  metrics->addWidget(resolution_, 0, 1, Qt::AlignRight);
  metrics->addWidget(metricKey(QStringLiteral("Codec"), block), 0, 2);
  metrics->addWidget(codec_, 0, 3, Qt::AlignRight);
  metrics->addWidget(metricKey(QStringLiteral("Frame rate"), block), 1, 0);
  metrics->addWidget(frameRate_, 1, 1, Qt::AlignRight);
  metrics->addWidget(metricKey(QStringLiteral("Bitrate"), block), 1, 2);
  metrics->addWidget(bitrate_, 1, 3, Qt::AlignRight);
  metrics->setColumnStretch(1, 1);
  metrics->setColumnStretch(3, 1);
  right->addLayout(metrics);

  testError_ = new QLabel(block);
  testError_->setProperty("tone", QStringLiteral("critical"));
  testError_->setWordWrap(true);
  testError_->hide();
  right->addWidget(testError_);
  right->addStretch(1);

  auto* buttons = new QHBoxLayout();
  buttons->setContentsMargins(0, 0, 0, 0);
  buttons->setSpacing(8);
  testButton_ = new QPushButton(QStringLiteral("Test connection"), block);
  testButton_->setObjectName(QStringLiteral("TestConnection"));
  testButton_->setProperty("role", QStringLiteral("secondary"));
  Theme::setVariant(testButton_, "md");
  testButton_->setCursor(Qt::PointingHandCursor);
  copyButton_ = new QPushButton(QStringLiteral("Copy ffmpeg command"), block);
  copyButton_->setObjectName(QStringLiteral("CopyFfmpeg"));
  copyButton_->setProperty("role", QStringLiteral("ghost"));
  Theme::setVariant(copyButton_, "md");
  copyButton_->setCursor(Qt::PointingHandCursor);
  copyNote_ = hintLabel({}, block);
  buttons->addWidget(testButton_);
  buttons->addWidget(copyButton_);
  buttons->addWidget(copyNote_);
  buttons->addStretch(1);
  right->addLayout(buttons);
  row->addLayout(right, 1);
  column->addWidget(block);

  connect(testButton_, &QPushButton::clicked, this, &ConnectionPage::runTest);
  connect(copyButton_, &QPushButton::clicked, this, &ConnectionPage::copyCommand);
}

void ConnectionPage::buildToggles() {
  auto* column = static_cast<QVBoxLayout*>(layout());
  auto* rows = new QVBoxLayout();
  rows->setContentsMargins(0, 0, 0, 0);
  rows->setSpacing(10);

  auto* analyticsRow = new QHBoxLayout();
  analyticsRow->setContentsMargins(0, 0, 0, 0);
  analyticsRow->setSpacing(10);
  analyticsRow->addWidget(new QLabel(QStringLiteral("Run analytics on this camera"), this));
  analyticsRow->addWidget(hintLabel(QStringLiteral("no effect until analytics ships"), this));
  analyticsRow->addStretch(1);
  analytics_ = new ToggleSwitch(ToggleSwitch::Size::Md, this);
  analytics_->setObjectName(QStringLiteral("AnalyticsToggle"));
  analyticsRow->addWidget(analytics_);
  rows->addLayout(analyticsRow);

  auto* recordRow = new QHBoxLayout();
  recordRow->setContentsMargins(0, 0, 0, 0);
  recordRow->setSpacing(10);
  recordRow->addWidget(new QLabel(QStringLiteral("Record continuously"), this));
  recordRow->addStretch(1);
  record_ = new ToggleSwitch(ToggleSwitch::Size::Md, this);
  record_->setObjectName(QStringLiteral("RecordToggle"));
  recordRow->addWidget(record_);
  rows->addLayout(recordRow);
  column->addLayout(rows);
}

bool ConnectionPage::eventFilter(QObject* watched, QEvent* event) {
  if (watched == password_ && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
    passwordField_->setProperty("focus", event->type() == QEvent::FocusIn);
    restyle(passwordField_);
  }
  return QWidget::eventFilter(watched, event);
}

QString ConnectionPage::kind() const { return protocol_->currentData().toString(); }
QString ConnectionPage::transport() const { return transport_->currentData().toString(); }

void ConnectionPage::applyProtocol() {
  const bool file = kind() == QLatin1StringView("file");
  mainUrlLabel_->setText(file ? QStringLiteral("File path") : QStringLiteral("Main stream URL"));
  mainUrl_->setPlaceholderText(file ? QStringLiteral("/recordings/sample.mkv")
                                    : QStringLiteral("rtsp://10.4.18.62:554/Streaming/Channels/101"));
  browse_->setVisible(file);
  subUrl_->setEnabled(!file);
  username_->setEnabled(!file);
  password_->setEnabled(!file);
  reveal_->setEnabled(!file);
  transport_->setEnabled(!file);
}

void ConnectionPage::browseFile() {
  const QString path = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose a video file"), mainUrl_->text(),
                                                    QStringLiteral("Video files (*.mkv *.mp4 *.mov *.ts);;All files (*)"));
  if (!path.isEmpty()) mainUrl_->setText(path);
}

void ConnectionPage::load(const fovea::Camera& camera, const QStringList& groups, bool existing) {
  name_->setText(camera.name);
  group_->clear();
  group_->addItems(groups);
  group_->setEditText(camera.groupName);
  code_->setText(camera.code);
  protocol_->setCurrentIndex(std::max(0, protocol_->findData(camera.kind)));
  mainUrl_->setText(camera.mainUrl);
  subUrl_->setText(camera.subUrl);
  transport_->setCurrentIndex(std::max(0, transport_->findData(camera.transport)));
  timeout_->setValue(std::clamp(camera.timeoutMs / 1000, timeout_->minimum(), timeout_->maximum()));
  jitter_->setValue(std::clamp(camera.jitterMs, jitter_->minimum(), jitter_->maximum()));
  analytics_->setChecked(camera.analyticsEnabled);
  record_->setChecked(camera.recordEnabled);
  username_->clear();
  password_->clear();
  replacesStoredCredentials_ = existing;
  username_->setPlaceholderText(existing ? QStringLiteral("stored") : QStringLiteral("svc_vms"));
  password_->setPlaceholderText(existing ? QStringLiteral("stored") : QString());
  applyProtocol();
  name_->setFocus();
}

QJsonObject ConnectionPage::formJson() const {
  QJsonObject o{{"name", name_->text().trimmed()},
                {"code", code_->text().trimmed()},
                {"group_name", group_->currentText().trimmed()},
                {"kind", kind()},
                {"main_url", mainUrl_->text().trimmed()},
                {"sub_url", subUrl_->text().trimmed()},
                {"transport", transport()},
                {"timeout_ms", timeout_->value() * 1000},
                {"jitter_ms", jitter_->value()},
                {"analytics_enabled", analytics_->isChecked()},
                {"record_enabled", record_->isChecked()}};
  if (!username_->text().isEmpty() || !password_->text().isEmpty()) {
    o.insert("username", username_->text());
    o.insert("password", password_->text());
  }
  return o;
}

QString ConnectionPage::validate(QWidget** focusTarget) const {
  if (name_->text().trimmed().isEmpty()) {
    *focusTarget = name_;
    return QStringLiteral("Display name is required.");
  }
  if (code_->text().trimmed().isEmpty()) {
    *focusTarget = code_;
    return QStringLiteral("Code is required.");
  }
  if (mainUrl_->text().trimmed().isEmpty()) {
    *focusTarget = mainUrl_;
    return kind() == QLatin1StringView("file") ? QStringLiteral("File path is required.")
                                               : QStringLiteral("Main stream URL is required.");
  }
  // The service stores username and password as one pair, so a lone field would clear the other.
  if (replacesStoredCredentials_ && username_->text().isEmpty() != password_->text().isEmpty()) {
    *focusTarget = username_->text().isEmpty() ? username_ : password_;
    return QStringLiteral("Enter both username and password to replace the stored credentials.");
  }
  return {};
}

QString ConnectionPage::displayName() const { return name_->text().trimmed(); }

void ConnectionPage::setTestState(TestState state, const QString& detail) {
  switch (state) {
    case TestState::Idle: testChip_->set(QStringLiteral("NOT TESTED"), StatusChip::Tone::Neutral, false); break;
    case TestState::Testing: testChip_->set(QStringLiteral("TESTING"), StatusChip::Tone::Info, true); break;
    case TestState::Connected: testChip_->set(QStringLiteral("CONNECTED"), StatusChip::Tone::Positive, true); break;
    case TestState::Failed: testChip_->set(QStringLiteral("FAILED"), StatusChip::Tone::Critical, true); break;
  }
  handshake_->setText(state == TestState::Connected ? detail : QString());
  testError_->setText(state == TestState::Failed ? detail : QString());
  testError_->setVisible(state == TestState::Failed);
  testButton_->setEnabled(state != TestState::Testing);
}

void ConnectionPage::showTestResult(const fovea::ConnectionTest& result) {
  if (!result.ok) {
    resolution_->setText(kDash);
    codec_->setText(kDash);
    frameRate_->setText(kDash);
    bitrate_->setText(kDash);
    setTestState(TestState::Failed, result.error.isEmpty() ? QStringLiteral("The camera did not answer.") : result.error);
    return;
  }
  resolution_->setText(result.width > 0 && result.height > 0 ? QStringLiteral("%1×%2").arg(result.width).arg(result.height) : kDash);
  codec_->setText(result.codec.isEmpty() ? kDash : codecLabel(result.codec));
  frameRate_->setText(result.fps > 0 ? QStringLiteral("%1 fps").arg(QString::number(result.fps, 'f', result.fps == std::floor(result.fps) ? 0 : 1)) : kDash);
  bitrate_->setText(bitrateLabel(result.bitrateKbps));
  setTestState(TestState::Connected, QStringLiteral("handshake %1 ms").arg(result.handshakeMs));
}

void ConnectionPage::runTest() {
  if (testInFlight_) return;
  if (mainUrl_->text().trimmed().isEmpty()) {
    setTestState(TestState::Failed, kind() == QLatin1StringView("file") ? QStringLiteral("Enter a file path first.")
                                                                         : QStringLiteral("Enter the main stream URL first."));
    mainUrl_->setFocus();
    return;
  }
  testInFlight_ = true;
  setTestState(TestState::Testing, {});
  client_.testConnection(formJson(), [this](bool ok, const QJsonDocument& doc, const QString& error) {
    testInFlight_ = false;
    if (!ok) {
      fovea::ConnectionTest failed;
      failed.error = error;
      showTestResult(failed);
      return;
    }
    showTestResult(fovea::ConnectionTest::fromJson(doc.object()));
  }, this);
}

void ConnectionPage::copyCommand() {
  StreamSource source{kind(), mainUrl_->text().trimmed(), username_->text(), password_->text(), transport()};
  if (source.url.isEmpty()) {
    copyNote_->setText(QStringLiteral("nothing to copy · enter a URL first"));
  } else {
    QApplication::clipboard()->setText(ffplayCommand(source));
    copyNote_->setText(source.password.isEmpty() ? QStringLiteral("copied")
                                                 : QStringLiteral("copied · the command contains the password"));
  }
  QTimer::singleShot(kCopyNoteMs, copyNote_, [this] { copyNote_->clear(); });
}

}

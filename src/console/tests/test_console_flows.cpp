// Drives the console's service client, event store, alert dispatcher and rule
// editor in process against console-stub-core: operator actions, zone and rule
// saves, and delivery of a live alert. The stub's data is a TEST FIXTURE.
#include "alerts/AlertLogic.h"
#include "core/AlertDispatcher.h"
#include "core/CoreClient.h"
#include "core/EventStore.h"
#include "core/StatusPoller.h"
#include "screens/alerts/RuleEditor.h"
#include "screens/alerts/ZoneCanvas.h"
#include "fovea/Clock.h"
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <memory>

using namespace fovea::ui;

namespace {
constexpr int kStubStartMs = 15000;
constexpr int kLoadMs = 10000;
constexpr int64_t kSecond = 1'000'000'000;
}

class TestConsoleFlows : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void storeLoadsFixtures();
  void operatorActionsReachTheService();
  void ruleEditorCreatesZoneAndRule();
  void ruleEditorRevisesRuleKeepingZone();
  void dispatcherShowsPopsAndConfirmsLiveAlert();
  void cleanupTestCase();

private:
  void saveEditor(RuleEditor& editor, QString* savedId);
  void setEventsDelay(int ms);

  QTemporaryDir dir_;
  QProcess stub_;
  std::unique_ptr<CoreClient> client_;
  std::unique_ptr<EventStore> store_;
  std::unique_ptr<StatusPoller> poller_;
  QVector<fovea::Camera> cameras_;
  QVector<fovea::CameraStatus> statuses_;
};

void TestConsoleFlows::initTestCase() {
  QVERIFY(dir_.isValid());
  stub_.setProgram(QStringLiteral(FOVEA_STUB_CORE_BIN));
  stub_.setArguments({QStringLiteral("--data-dir"), dir_.path()});
  stub_.setProcessChannelMode(QProcess::ForwardedChannels);
  stub_.start();
  QVERIFY2(stub_.waitForStarted(5000), qPrintable(stub_.errorString()));
  QElapsedTimer t;
  t.start();
  while (!QFile::exists(dir_.path() + QStringLiteral("/core.json")) && t.elapsed() < kStubStartMs) QTest::qWait(100);
  QVERIFY(QFile::exists(dir_.path() + QStringLiteral("/core.json")));
  qputenv("FOVEA_DATA_DIR", dir_.path().toLocal8Bit());

  client_ = std::make_unique<CoreClient>();
  store_ = std::make_unique<EventStore>(*client_);
  poller_ = std::make_unique<StatusPoller>(*client_);
  connect(poller_.get(), &StatusPoller::snapshot, this,
          [this](const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses) {
    cameras_ = cameras;
    statuses_ = statuses;
  });
  poller_->setActive(true);
  store_->setActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(store_->eventsLoaded() && store_->rulesLoaded() && cameras_.size() == 2, kLoadMs);
}

void TestConsoleFlows::storeLoadsFixtures() {
  QCOMPARE(store_->rules().size(), 3);
  QCOMPARE(store_->zones().size(), 2);
  QCOMPARE(store_->events().size(), 5);
  QCOMPARE(store_->unresolvedCount(), 2);
  for (int i = 1; i < store_->events().size(); ++i)
    QVERIFY(store_->events()[i - 1].openedUtcMs >= store_->events()[i].openedUtcMs);
}

void TestConsoleFlows::operatorActionsReachTheService() {
  QString id;
  for (const EventInfo& e : store_->events())
    if (e.operatorState == QLatin1StringView("new") && e.evidence && e.evidence->state == QLatin1StringView("available")) id = e.id;
  QVERIFY(!id.isEmpty());
  QSignalSpy updated(store_.get(), &EventStore::eventUpdated);
  store_->acknowledge(id);
  QTRY_VERIFY_WITH_TIMEOUT(store_->findEvent(id) && store_->findEvent(id)->operatorState == QLatin1StringView("acknowledged"), kLoadMs);
  QTRY_COMPARE(updated.count(), 1);
  QCOMPARE(alertBucket(*store_->findEvent(id)), AlertBucket::Acknowledged);
  store_->review(id, QStringLiteral("false_alarm"));
  QTRY_VERIFY_WITH_TIMEOUT(store_->findEvent(id) && store_->findEvent(id)->review, kLoadMs);
  QCOMPARE(store_->findEvent(id)->review->label, QStringLiteral("false_alarm"));
  QCOMPARE(alertBucket(*store_->findEvent(id)), AlertBucket::Dismissed);
  QCOMPARE(store_->unresolvedCount(), 1);
}

void TestConsoleFlows::saveEditor(RuleEditor& editor, QString* savedId) {
  QSignalSpy saved(&editor, &RuleEditor::saved);
  auto* save = editor.findChild<QPushButton*>(QStringLiteral("RuleSave"));
  QVERIFY(save);
  QTest::mouseClick(save, Qt::LeftButton);
  QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, kLoadMs);
  *savedId = saved.at(0).at(0).toString();
}

void TestConsoleFlows::ruleEditorCreatesZoneAndRule() {
  RuleEditor editor(*client_, *store_);
  editor.resize(376, 1200);
  editor.load(std::nullopt);
  editor.setCameras(cameras_, statuses_);
  editor.show();
  auto* canvas = editor.findChild<ZoneCanvas*>();
  QVERIFY(canvas);
  QTRY_VERIFY_WITH_TIMEOUT(canvas->hasFrame(), kLoadMs);
  editor.findChild<QLineEdit*>(QStringLiteral("RuleName"))->setText(QStringLiteral("Flow test rule"));
  const int w = canvas->width();
  const int h = canvas->height();
  for (const auto& [fx, fy] : {std::pair{0.3, 0.3}, std::pair{0.7, 0.3}, std::pair{0.7, 0.8}, std::pair{0.3, 0.8}})
    QTest::mouseClick(canvas, Qt::LeftButton, {}, QPoint(static_cast<int>(w * fx), static_cast<int>(h * fy)));
  QCOMPARE(canvas->points().size(), 4);
  QCOMPARE(canvas->referenceSize(), QSize(640, 360));

  QString id;
  saveEditor(editor, &id);
  QVERIFY(!id.isEmpty());
  QTRY_VERIFY_WITH_TIMEOUT(store_->findRule(id), kLoadMs);
  const RuleInfo rule = *store_->findRule(id);
  QCOMPARE(rule.name, QStringLiteral("Flow test rule"));
  QCOMPARE(rule.cameraId, cameras_.first().id);
  QCOMPARE(rule.timeZone, QString::fromUtf8(QTimeZone::systemTimeZoneId()));
  QVERIFY(rule.soundAction && rule.popAction && rule.enabled);
  const ZoneInfo* zone = store_->findZone(rule.zoneId);
  QVERIFY(zone);
  QCOMPARE(zone->cameraId, rule.cameraId);
  QCOMPARE(zone->points.size(), 4);
  QCOMPARE(zone->refWidth, 640);
  QCOMPARE(zone->refHeight, 360);
  QCOMPARE(rule.zoneRevision, zone->revision);
}

void TestConsoleFlows::ruleEditorRevisesRuleKeepingZone() {
  const RuleInfo original = store_->rules().first();
  const ZoneInfo zoneBefore = *store_->findZone(original.zoneId);
  RuleEditor editor(*client_, *store_);
  editor.resize(376, 1200);
  editor.load(original);
  editor.setCameras(cameras_, statuses_);
  editor.show();
  auto* canvas = editor.findChild<ZoneCanvas*>();
  QTRY_VERIFY_WITH_TIMEOUT(canvas->hasFrame(), kLoadMs);
  QCOMPARE(canvas->points(), zoneBefore.points);
  editor.findChild<QDoubleSpinBox*>(QStringLiteral("RuleDwell"))->setValue(15.5);

  QString id;
  saveEditor(editor, &id);
  QCOMPARE(id, original.id);
  QTRY_VERIFY_WITH_TIMEOUT(store_->findRule(id) && store_->findRule(id)->revision == original.revision + 1, kLoadMs);
  const RuleInfo revised = *store_->findRule(id);
  QCOMPARE(revised.dwellNs, 15 * kSecond + kSecond / 2);
  QCOMPARE(revised.minConfidence, original.minConfidence);
  QVERIFY(!editor.findChild<QComboBox*>(QStringLiteral("RuleCamera"))->isEnabled());
  QCOMPARE(revised.zoneId, original.zoneId);
  QCOMPARE(revised.zoneRevision, zoneBefore.revision);
  QCOMPARE(revised.schedule.first().days, original.schedule.first().days);
  QCOMPARE(revised.schedule.first().startMinute, original.schedule.first().startMinute);
  QCOMPARE(revised.schedule.first().endMinute, original.schedule.first().endMinute);
  QCOMPARE(revised.timeZone, original.timeZone);
  QCOMPARE(revised.preserved.value("rearm_ns").toDouble(), original.preserved.value("rearm_ns").toDouble());
}

void TestConsoleFlows::setEventsDelay(int ms) {
  QFile tokenFile(dir_.path() + QStringLiteral("/core.token"));
  QVERIFY(tokenFile.open(QIODevice::ReadOnly));
  QNetworkRequest request(QUrl(client_->baseUrl() + QStringLiteral("/v1/test/events-delay")));
  request.setRawHeader("Authorization", "Bearer " + tokenFile.readAll().trimmed());
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  QNetworkAccessManager network;
  QNetworkReply* reply = network.post(request, QJsonDocument(QJsonObject{{"ms", ms}}).toJson(QJsonDocument::Compact));
  QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), kLoadMs);
  QCOMPARE(reply->error(), QNetworkReply::NoError);
  reply->deleteLater();
}

// The event list answers 2.5 s late, so later polls list the delivery while it is
// still being presented; it must be confirmed only after it was presented.
void TestConsoleFlows::dispatcherShowsPopsAndConfirmsLiveAlert() {
  setEventsDelay(2500);
  AlertDispatcher dispatcher(*client_, *store_);
  QStringList shown;
  QStringList popped;
  QHash<QString, int64_t> presentedUtcMs;
  connect(&dispatcher, &AlertDispatcher::alertPresented, this, [&shown, &presentedUtcMs](const QString& id) {
    shown << id;
    presentedUtcMs.insert(id, fovea::utcNowMs());
  });
  connect(&dispatcher, &AlertDispatcher::popCameraRequested, this, [&popped](const QString& id) { popped << id; });
  dispatcher.setActive(true);

  auto liveEvent = [this, &shown]() -> const EventInfo* {
    for (const QString& id : shown) {
      const EventInfo* e = store_->findEvent(id);
      if (e && e->detail.contains(QStringLiteral("live alert"))) return e;
    }
    return nullptr;
  };
  QTRY_VERIFY_WITH_TIMEOUT(liveEvent(), kLoadMs);
  const QString liveId = liveEvent()->id;
  const QString liveCamera = liveEvent()->cameraId;
  QVERIFY(popped.contains(liveCamera));
  QCOMPARE(shown.count(liveId), 1);

  auto consoleDelivered = [this, &liveId] {
    const EventInfo* e = store_->findEvent(liveId);
    if (!e) return false;
    for (const DeliveryInfo& d : e->deliveries)
      if (d.channel == QLatin1StringView("console")) return d.state == QLatin1StringView("delivered");
    return false;
  };
  QTRY_VERIFY_WITH_TIMEOUT(consoleDelivered(), kLoadMs);
  const EventInfo* live = store_->findEvent(liveId);
  QString soundState;
  for (const DeliveryInfo& d : live->deliveries) {
    if (d.channel == QLatin1StringView("sound")) soundState = d.state;
    if (d.channel == QLatin1StringView("console")) QVERIFY(d.deliveredUtcMs >= presentedUtcMs.value(liveId));
  }
  qInfo("sound delivery for the live alert: %s", qPrintable(soundState));
  setEventsDelay(0);
}

void TestConsoleFlows::cleanupTestCase() {
  store_.reset();
  poller_.reset();
  client_.reset();
  if (stub_.state() == QProcess::NotRunning) return;
  stub_.terminate();
  if (!stub_.waitForFinished(5000)) {
    stub_.kill();
    stub_.waitForFinished(2000);
  }
}

QTEST_MAIN(TestConsoleFlows)
#include "test_console_flows.moc"

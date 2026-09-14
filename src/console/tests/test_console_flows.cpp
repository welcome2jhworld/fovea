// Drives the console's service client, event store, alert dispatcher, rule
// editor and search screen in process against console-stub-core: operator
// actions, zone and rule saves, delivery of a live alert, and search results,
// cancellation, filters and the empty and error states. The stub's data is a
// TEST FIXTURE.
#include "alerts/AlertLogic.h"
#include "core/AlertDispatcher.h"
#include "core/CoreClient.h"
#include "core/EventStore.h"
#include "core/StatusPoller.h"
#include "core/ThumbnailCache.h"
#include "screens/alerts/RuleEditor.h"
#include "screens/MainTabBar.h"
#include "screens/MainWindow.h"
#include "screens/alerts/ZoneCanvas.h"
#include "screens/search/ResultGrid.h"
#include "screens/search/SearchScreen.h"
#include "fovea/Clock.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QLabel>
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
  void searchShowsRankedResultsAndStats();
  void searchCancelsTheRunningQuery();
  void searchSendsTimeAndCameraFilters();
  void searchEmptyAndErrorStatesOfferOneAction();
  void searchKeyboardMovesSelectionAndTogglesPlayback();
  void commandKOpensSearchFromAnotherScreen();
  void cleanupTestCase();

private:
  void saveEditor(RuleEditor& editor, QString* savedId);
  void setEventsDelay(int ms);
  QJsonObject getJson(const QString& path);

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

QJsonObject TestConsoleFlows::getJson(const QString& path) {
  QFile tokenFile(dir_.path() + QStringLiteral("/core.token"));
  if (!tokenFile.open(QIODevice::ReadOnly)) return {};
  QNetworkRequest request(QUrl(client_->baseUrl() + path));
  request.setRawHeader("Authorization", "Bearer " + tokenFile.readAll().trimmed());
  QNetworkAccessManager network;
  QNetworkReply* reply = network.get(request);
  QElapsedTimer t;
  t.start();
  while (!reply->isFinished() && t.elapsed() < kLoadMs) QTest::qWait(20);
  const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
  reply->deleteLater();
  return object;
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

void TestConsoleFlows::searchShowsRankedResultsAndStats() {
  ThumbnailCache thumbnails(*client_, ThumbnailCache::Source::SearchRecord);
  SearchScreen screen(*client_, thumbnails);
  screen.setCameras(cameras_);
  screen.search(QStringLiteral("person walking across the gate apron"));
  QCOMPARE(screen.state(), SearchScreen::State::Searching);
  QTRY_COMPARE_WITH_TIMEOUT(screen.state(), SearchScreen::State::Results, kLoadMs);

  const SearchResponseInfo& response = screen.response();
  QCOMPARE(response.results.size(), 8);
  QCOMPARE(indexVersionLabel(response), QStringLiteral("siglip2-b16-224 · 5f0c2a9e41d7"));
  for (int i = 1; i < response.results.size(); ++i) QVERIFY(response.results[i - 1].relevance >= response.results[i].relevance);
  for (const SearchResultInfo& r : response.results) {
    QVERIFY(!r.recordId.isEmpty());
    QVERIFY(r.endUtcMs > r.startUtcMs);
  }
  const auto* stats = screen.findChild<QLabel*>(QStringLiteral("SearchStats"));
  QVERIFY(stats->text().contains(QStringLiteral("8 results")));
  QVERIFY(stats->text().contains(QStringLiteral("<span style=\"color:#E2A43C\">coverage 87%</span>")));
  QVERIFY(stats->toolTip().contains(QStringLiteral("not indexed")));
  const auto* relevance = screen.findChild<QLabel*>(QStringLiteral("InspectorRelevance"));
  QCOMPARE(relevance->text(), QStringLiteral("relevance 0.31"));

  const QString record = response.results.first().recordId;
  QTRY_VERIFY_WITH_TIMEOUT(!thumbnails.thumbnail(record).isNull(), kLoadMs);
  QCOMPARE(thumbnails.thumbnail(record).width(), 640);

  const QJsonObject session = getJson(QStringLiteral("/v1/search/%1").arg(response.sessionId));
  QCOMPARE(session.value("query").toString(), QStringLiteral("person walking across the gate apron"));
  QCOMPARE(session.value("results").toArray().size(), 8);
}

// The slow query is aborted synchronously when the next one starts, so the number of
// outstanding requests does not grow, and the fast answer arrives without waiting for it.
void TestConsoleFlows::searchCancelsTheRunningQuery() {
  ThumbnailCache thumbnails(*client_, ThumbnailCache::Source::SearchRecord);
  SearchScreen screen(*client_, thumbnails);
  screen.setCameras(cameras_);
  QSignalSpy states(&screen, &SearchScreen::stateChanged);
  screen.search(QStringLiteral("person walking through the lobby [slow]"));
  const int withSlowQuery = client_->pendingRequests();
  QElapsedTimer t;
  t.start();
  screen.search(QStringLiteral("person walking across the gate apron"));
  QCOMPARE(client_->pendingRequests(), withSlowQuery);
  QTRY_COMPARE_WITH_TIMEOUT(screen.state(), SearchScreen::State::Results, kLoadMs);
  QVERIFY2(t.elapsed() < 5000, qPrintable(QStringLiteral("results took %1 ms").arg(t.elapsed())));
  QCOMPARE(screen.response().results.size(), 8);
  QTest::qWait(300);
  QCOMPARE(screen.state(), SearchScreen::State::Results);
  QCOMPARE(states.count(), 2);
}

void TestConsoleFlows::searchSendsTimeAndCameraFilters() {
  ThumbnailCache thumbnails(*client_, ThumbnailCache::Source::SearchRecord);
  SearchScreen screen(*client_, thumbnails);
  screen.setCameras(cameras_);
  const QString lobby = cameras_[1].id;
  screen.setCameraFilter({lobby});
  screen.search(QStringLiteral("person walking across the gate apron"));
  QTRY_COMPARE_WITH_TIMEOUT(screen.state(), SearchScreen::State::Results, kLoadMs);
  QCOMPARE(screen.response().results.size(), 4);
  for (const SearchResultInfo& r : screen.response().results) QCOMPARE(r.cameraId, lobby);
  const QJsonObject filters = getJson(QStringLiteral("/v1/search/%1").arg(screen.response().sessionId)).value("filters").toObject();
  QCOMPARE(filters.value("camera_ids").toArray(), QJsonArray{lobby});
  QCOMPARE(static_cast<int64_t>(filters.value("to_utc_ms").toDouble() - filters.value("from_utc_ms").toDouble()), kDayMs);
  QCOMPARE(filters.value("limit").toInt(), SearchScreen::kResultLimit);
  QCOMPARE(filters.value("min_gap_ms").toInt(), SearchScreen::kMinGapMs);

  screen.setCameraFilter({});
  screen.setRangePreset(RangePreset::LastHour);
  screen.search(QStringLiteral("person walking across the gate apron"));
  QTRY_COMPARE_WITH_TIMEOUT(screen.state(), SearchScreen::State::Results, kLoadMs);
  QCOMPARE(screen.response().results.size(), 6);
  const QJsonObject hourFilters = getJson(QStringLiteral("/v1/search/%1").arg(screen.response().sessionId)).value("filters").toObject();
  QCOMPARE(hourFilters.value("camera_ids").toArray().size(), cameras_.size());
  QCOMPARE(static_cast<int64_t>(hourFilters.value("to_utc_ms").toDouble() - hourFilters.value("from_utc_ms").toDouble()), kHourMs);
}

void TestConsoleFlows::searchEmptyAndErrorStatesOfferOneAction() {
  ThumbnailCache thumbnails(*client_, ThumbnailCache::Source::SearchRecord);
  SearchScreen screen(*client_, thumbnails);
  screen.setCameras(cameras_);
  auto* action = screen.findChild<QPushButton*>(QStringLiteral("SearchMessageAction"));
  auto* detail = screen.findChild<QLabel*>(QStringLiteral("SearchMessageDetail"));

  screen.search(QStringLiteral("red umbrella left on a bench [empty]"));
  QTRY_COMPARE_WITH_TIMEOUT(screen.state(), SearchScreen::State::Empty, kLoadMs);
  QVERIFY(action->isVisibleTo(&screen));
  QCOMPARE(action->text(), QStringLiteral("Search the last 7 days"));
  QVERIFY(!screen.findChild<QLabel*>(QStringLiteral("SearchMessage"))->text().isEmpty());
  action->click();
  QCOMPARE(screen.state(), SearchScreen::State::Searching);
  QTRY_COMPARE_WITH_TIMEOUT(screen.state(), SearchScreen::State::Empty, kLoadMs);
  const QJsonObject filters = getJson(QStringLiteral("/v1/search/%1").arg(screen.response().sessionId)).value("filters").toObject();
  QCOMPARE(static_cast<int64_t>(filters.value("to_utc_ms").toDouble() - filters.value("from_utc_ms").toDouble()), kWeekMs);
  QCOMPARE(action->text(), QStringLiteral("Edit the query"));

  screen.search(QStringLiteral("person at the turnstiles [error]"));
  QTRY_COMPARE_WITH_TIMEOUT(screen.state(), SearchScreen::State::Error, kLoadMs);
  QCOMPARE(action->text(), QStringLiteral("Retry"));
  QVERIFY(detail->text().contains(QStringLiteral("503")));
  QVERIFY(detail->text().contains(QStringLiteral("did not answer")));
  QSignalSpy states(&screen, &SearchScreen::stateChanged);
  action->click();
  QTRY_COMPARE_WITH_TIMEOUT(screen.state(), SearchScreen::State::Error, kLoadMs);
  QCOMPARE(states.count(), 2);
}

void TestConsoleFlows::searchKeyboardMovesSelectionAndTogglesPlayback() {
  ThumbnailCache thumbnails(*client_, ThumbnailCache::Source::SearchRecord);
  SearchScreen screen(*client_, thumbnails);
  screen.setCameras(cameras_);
  screen.resize(1580, 1000);
  screen.show();
  QVERIFY(QTest::qWaitForWindowExposed(&screen));
  auto* query = screen.findChild<QLineEdit*>(QStringLiteral("QueryField"));
  auto* grid = screen.findChild<ResultGrid*>();
  screen.focusQuery();
  QTest::keyClicks(query, QStringLiteral("person walking across the gate apron"));
  QTest::keyClick(query, Qt::Key_Return);
  QTRY_COMPARE_WITH_TIMEOUT(screen.state(), SearchScreen::State::Results, kLoadMs);
  QCOMPARE(grid->currentIndex().row(), 0);
  const auto* relevance = screen.findChild<QLabel*>(QStringLiteral("InspectorRelevance"));
  QCOMPARE(relevance->text(), QStringLiteral("relevance 0.31"));
  QTest::keyClick(grid, Qt::Key_Right);
  QCOMPARE(grid->currentIndex().row(), 1);
  QCOMPARE(relevance->text(), QStringLiteral("relevance 0.30"));
  QTest::keyClick(grid, Qt::Key_Down);
  QCOMPARE(grid->currentIndex().row(), 5);
  QSignalSpy toggles(grid, &ResultGrid::playbackToggleRequested);
  QTest::keyClick(grid, Qt::Key_Space);
  QCOMPARE(toggles.count(), 1);
  QCOMPARE(grid->currentIndex().row(), 5);
}

// Last: the window is a new console session, for which the stub raises one more live alert.
void TestConsoleFlows::commandKOpensSearchFromAnotherScreen() {
  MainWindow window;
  window.resize(1440, 900);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));
  window.activateWindow();
  QVERIFY(QTest::qWaitForWindowActive(&window));
  auto* tabs = window.findChild<NavTabs*>();
  auto* query = window.findChild<QLineEdit*>(QStringLiteral("QueryField"));
  QVERIFY(tabs && query);
  tabs->setCurrentIndex(2);
  QTest::keyClick(&window, Qt::Key_K, Qt::ControlModifier);
  QCOMPARE(tabs->currentIndex(), 1);
  QTRY_VERIFY_WITH_TIMEOUT(query->hasFocus(), kLoadMs);
  query->setText(QStringLiteral("typed"));
  tabs->setCurrentIndex(0);
  QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &window, Qt::Key_K, Qt::ControlModifier);
  QCOMPARE(tabs->currentIndex(), 1);
  QTRY_VERIFY_WITH_TIMEOUT(query->hasFocus(), kLoadMs);
  QCOMPARE(query->selectedText(), QStringLiteral("typed"));
  window.close();
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

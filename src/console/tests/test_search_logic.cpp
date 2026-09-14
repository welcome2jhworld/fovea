#include "core/SearchTypes.h"
#include "search/SearchLogic.h"
#include <QJsonDocument>
#include <QTest>

using namespace fovea::ui;

namespace {
SearchStatsInfo statsWith(double hours, std::optional<double> coverage, int64_t totalMs, int64_t samples = 100) {
  SearchStatsInfo s;
  s.hoursScanned = hours;
  s.coverageRatio = coverage;
  s.totalMs = totalMs;
  s.samplesScanned = samples;
  return s;
}

// The shape fovea-core serialises (SearchRange::toJson in src/core/src/Index.cpp).
const char* const kCoreResponse = R"({
  "session_id": "5c7e2f4a-0000-4000-8000-000000000001",
  "results": [{
    "camera_id": "cam-a", "start_utc_ms": 1757800000000, "end_utc_ms": 1757800008000, "relevance": 0.3123,
    "representative": {"record_id": 42, "camera_id": "cam-a", "segment_id": "seg-1", "utc_ms": 1757800004000,
                       "relevance": 0.3123, "thumbnail": "/v1/search/thumbnails/42"},
    "samples": [{"record_id": 41}, {"record_id": 42}, {"record_id": 43}],
    "evidence_state": "partial"}],
  "stats": {"samples_scanned": 7200, "hours_scanned": 2, "coverage_ratio": 0.999, "embed_ms": 120, "scan_ms": 30,
            "total_ms": 160},
  "index_version": "a1b2c3d4e5f6", "index_version_name": "siglip2-b16-224", "model": "google/siglip2-base-patch16-224",
  "scoring": "embedding_similarity", "note": "similarity only, not verified"})";
}

class TestSearchLogic : public QObject {
  Q_OBJECT
private slots:
  void relevanceIsAScoreNotAPercentage();
  void coverageNeverRoundsUpToComplete();
  void statsLineAndCoverageExplanation();
  void hoursAndElapsed();
  void resultLengths();
  void emptyStateOffersOneAction();
  void rangesAndCameraLabels();
  void parsesCoreSearchResponse();
  void thumbnailPathStandsInForRecordId();
  void indexStatusLineListsCamerasAndQueue();
};

void TestSearchLogic::relevanceIsAScoreNotAPercentage() {
  QCOMPARE(relevanceLabel(0.3123), QStringLiteral("relevance 0.31"));
  QCOMPARE(relevanceLabel(0.968), QStringLiteral("relevance 0.97"));
  QCOMPARE(relevanceLabel(0.0), QStringLiteral("relevance 0.00"));
  for (double v : {0.0, 0.05, 0.5, 0.96, 1.0}) {
    const QString label = relevanceLabel(v);
    QVERIFY(!label.contains(QLatin1Char('%')));
    QVERIFY(!label.contains(QStringLiteral("match"), Qt::CaseInsensitive));
  }
}

void TestSearchLogic::coverageNeverRoundsUpToComplete() {
  QCOMPARE(coveragePercent(1.0), 100);
  QCOMPARE(coveragePercent(0.9999), 99);
  QCOMPARE(coveragePercent(0.87), 87);
  QCOMPARE(coveragePercent(0.0), 0);
  QCOMPARE(coveragePercent(1.4), 100);
  QCOMPARE(coveragePercent(-0.2), 0);
  QVERIFY(coverageIncomplete(statsWith(1, 0.9999, 10)));
  QVERIFY(!coverageIncomplete(statsWith(1, 1.0, 10)));
  QVERIFY(!coverageIncomplete(statsWith(1, std::nullopt, 10)));
}

void TestSearchLogic::statsLineAndCoverageExplanation() {
  QCOMPARE(statsLine(8, statsWith(24, 0.87, 740)), QStringLiteral("8 results · 24 hours searched · coverage 87% · 0.74 s"));
  QCOMPARE(statsLine(1, statsWith(1, 1.0, 3100)), QStringLiteral("1 result · 1 hour searched · coverage 100% · 3.1 s"));
  QCOMPARE(statsLine(0, statsWith(41.76, std::nullopt, 400)), QStringLiteral("0 results · 41.8 hours searched · 0.40 s"));
  QVERIFY(coverageExplanation(statsWith(24, 1.0, 1)).isEmpty());
  QVERIFY(coverageExplanation(statsWith(24, std::nullopt, 1)).isEmpty());
  const QString why = coverageExplanation(statsWith(24, 0.87, 1));
  QVERIFY(why.contains(QStringLiteral("13%")));
  QVERIFY(why.contains(QStringLiteral("not indexed")));
}

void TestSearchLogic::hoursAndElapsed() {
  QCOMPARE(hoursLabel(0), QStringLiteral("0 hours"));
  QCOMPARE(hoursLabel(0.03), QStringLiteral("< 0.1 hours"));
  QCOMPARE(hoursLabel(0.96), QStringLiteral("1 hour"));
  QCOMPARE(hoursLabel(2.25), QStringLiteral("2.3 hours"));
  QCOMPARE(hoursLabel(168), QStringLiteral("168 hours"));
  QCOMPARE(elapsedLabel(0), QStringLiteral("0.00 s"));
  QCOMPARE(elapsedLabel(412), QStringLiteral("0.41 s"));
  QCOMPARE(elapsedLabel(3140), QStringLiteral("3.1 s"));
  QCOMPARE(elapsedLabel(-5), QStringLiteral("0.00 s"));
}

void TestSearchLogic::resultLengths() {
  SearchResultInfo r;
  r.startUtcMs = 10'000;
  r.endUtcMs = 18'000;
  QCOMPARE(resultLengthLabel(r), QStringLiteral("0:08"));
  r.endUtcMs = 10'000 + 83'000;
  QCOMPARE(resultLengthLabel(r), QStringLiteral("1:23"));
  r.endUtcMs = r.startUtcMs;
  QCOMPARE(resultLengthLabel(r), QStringLiteral("1 sample"));
}

void TestSearchLogic::emptyStateOffersOneAction() {
  QCOMPARE(emptyActionFor(RangePreset::LastHour, true), EmptyAction::WidenRange);
  QCOMPARE(emptyActionFor(RangePreset::LastDay, false), EmptyAction::WidenRange);
  QCOMPARE(emptyActionFor(RangePreset::LastWeek, true), EmptyAction::AllCameras);
  QCOMPARE(emptyActionFor(RangePreset::Custom, false), EmptyAction::EditQuery);
  QCOMPARE(emptyActionLabel(EmptyAction::WidenRange), QStringLiteral("Search the last 7 days"));
  QVERIFY(emptySentence(statsWith(0, 0.0, 10, 0)).contains(QStringLiteral("nothing was searched")));
  QVERIFY(!emptySentence(statsWith(2, 0.5, 10, 3600)).contains(QStringLiteral("nothing was searched")));
}

void TestSearchLogic::rangesAndCameraLabels() {
  const int64_t now = 1'757'800'000'000;
  QCOMPARE(rangeFor(RangePreset::LastHour, now, {}).fromUtcMs, now - kHourMs);
  QCOMPARE(rangeFor(RangePreset::LastWeek, now, {}).toUtcMs, now);
  const TimeRange custom{now - 5000, now - 1000};
  QCOMPARE(rangeFor(RangePreset::Custom, now, custom).fromUtcMs, custom.fromUtcMs);
  QCOMPARE(rangeLabel(RangePreset::LastDay, {}), QStringLiteral("Last 24 hours"));
  QCOMPARE(cameraFilterLabel(0, 4, {}), QStringLiteral("All cameras"));
  QCOMPARE(cameraFilterLabel(4, 4, {}), QStringLiteral("All cameras"));
  QCOMPARE(cameraFilterLabel(1, 4, QStringLiteral("North Gate")), QStringLiteral("North Gate"));
  QCOMPARE(cameraFilterLabel(2, 4, {}), QStringLiteral("2 cameras"));
}

void TestSearchLogic::parsesCoreSearchResponse() {
  const SearchResponseInfo r = SearchResponseInfo::fromJson(QJsonDocument::fromJson(kCoreResponse).object());
  QCOMPARE(r.sessionId, QStringLiteral("5c7e2f4a-0000-4000-8000-000000000001"));
  QCOMPARE(r.indexVersion, QStringLiteral("a1b2c3d4e5f6"));
  QCOMPARE(indexVersionLabel(r), QStringLiteral("siglip2-b16-224 · a1b2c3d4e5f6"));
  QCOMPARE(r.model, QStringLiteral("google/siglip2-base-patch16-224"));
  QCOMPARE(r.results.size(), 1);
  const SearchResultInfo& first = r.results.first();
  QCOMPARE(first.cameraId, QStringLiteral("cam-a"));
  QCOMPARE(first.endUtcMs - first.startUtcMs, int64_t{8000});
  QCOMPARE(first.relevance, 0.3123);
  QCOMPARE(first.recordId, QStringLiteral("42"));
  QCOMPARE(first.representativeUtcMs, int64_t{1'757'800'004'000});
  QCOMPARE(first.samples, 3);
  QCOMPARE(first.evidenceState, QStringLiteral("partial"));
  QCOMPARE(r.stats.samplesScanned, int64_t{7200});
  QVERIFY(r.stats.coverageRatio.has_value());
  QCOMPARE(coveragePercent(*r.stats.coverageRatio), 99);
  QCOMPARE(r.stats.totalMs, int64_t{160});
}

void TestSearchLogic::thumbnailPathStandsInForRecordId() {
  const QJsonObject o = QJsonDocument::fromJson(R"({"camera_id": "c", "start_utc_ms": 5, "end_utc_ms": 2, "score": 0.2,
      "representative": {"utc_ms": 5, "thumbnail": "/v1/search/thumbnails/9001"}, "samples": 4})").object();
  const SearchResultInfo r = SearchResultInfo::fromJson(o);
  QCOMPARE(r.recordId, QStringLiteral("9001"));
  QCOMPARE(r.endUtcMs, r.startUtcMs);
  QCOMPARE(r.relevance, 0.2);
  QCOMPARE(r.samples, 4);
  QVERIFY(!SearchStatsInfo::fromJson(QJsonObject{}).coverageRatio.has_value());
}

void TestSearchLogic::indexStatusLineListsCamerasAndQueue() {
  const IndexStatusInfo status = IndexStatusInfo::fromJson(QJsonDocument::fromJson(R"({
      "active_index_version": "siglip2-b16-224", "model": "google/siglip2-base-patch16-224",
      "cameras": [{"camera_id": "a", "coverage_ratio": 1.0},
                  {"camera_id": "b", "frames_expected": 1000, "frames_indexed": 625},
                  {"camera_id": "c", "index_enabled": false},
                  {"camera_id": "d"}],
      "queue": {"queued": 3, "running": 1, "failed": 2}, "last_error": "sample: Could not demultiplex stream."})").object());
  QCOMPARE(status.cameras.size(), 4);
  QCOMPARE(status.lastError, QStringLiteral("sample: Could not demultiplex stream."));
  QCOMPARE(coveragePercent(*status.cameras[1].coverageRatio), 62);
  const QHash<QString, QString> names{{QStringLiteral("a"), QStringLiteral("North Gate")},
                                      {QStringLiteral("b"), QStringLiteral("Lobby")},
                                      {QStringLiteral("c"), QStringLiteral("Dock")}};
  QCOMPARE(fovea::ui::indexStatusLine(status, names),
           QStringLiteral("Index siglip2-b16-224 · North Gate 100% · Lobby 62% · Dock off · d — · 3 queued · 1 running · 2 failed"));
  const IndexStatusInfo idle = IndexStatusInfo::fromJson(QJsonDocument::fromJson(R"({"index_version": "v", "queue": 0})").object());
  QCOMPARE(fovea::ui::indexStatusLine(idle, {}), QStringLiteral("Index v · queue empty"));
  const IndexStatusInfo waiting = IndexStatusInfo::fromJson(QJsonDocument::fromJson(
      R"({"active_index_version": "v", "state": "waiting_worker", "queue": {"queued": 2}})").object());
  QCOMPARE(fovea::ui::indexStatusLine(waiting, {}), QStringLiteral("Index v · waiting for the model worker · 2 queued"));
}

QTEST_GUILESS_MAIN(TestSearchLogic)
#include "test_search_logic.moc"

// Starts console-stub-core, runs the console headless with FOVEA_SCREENSHOT,
// and checks that the wall shows the stub's moving pattern rather than the
// placeholder, that the M3 views render stub data (detection boxes, alert
// table, evidence playback, zone editor over a live frame), that the live
// alert reached the console and was confirmed, and that the Search tab draws
// its skeleton, results with a playing clip, empty and error states from the
// stub's fixture queries. Environment problems (no
// platform plugin, console cannot start) skip instead of failing unless
// FOVEA_SMOKE_STRICT=1.
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <cstdlib>

namespace {

constexpr int kStubStartMs = 15000;
constexpr int kConsoleRunMs = 60000;

bool strict() { return qEnvironmentVariableIntValue("FOVEA_SMOKE_STRICT") == 1; }

struct Region {
  double x0, y0, x1, y1;
};

// Pixels with any channel above 180: the stub pattern and primary text qualify,
// the stripe placeholder, muted labels and scrimmed chrome do not.
int brightPixels(const QImage& img, const Region& r) {
  int count = 0;
  const int x0 = static_cast<int>(img.width() * r.x0), x1 = static_cast<int>(img.width() * r.x1);
  const int y0 = static_cast<int>(img.height() * r.y0), y1 = static_cast<int>(img.height() * r.y1);
  for (int y = y0; y < y1; ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    for (int x = x0; x < x1; ++x) {
      const QRgb px = line[x];
      if (qRed(px) > 180 || qGreen(px) > 180 || qBlue(px) > 180) ++count;
    }
  }
  return count;
}

// Pixels within `tolerance` per channel of a colour: #E0603C marks detection boxes and label tabs,
// the stub playback pattern's (120,160,255) tells a playing clip from a still thumbnail.
int colourPixels(const QImage& img, const Region& r, QRgb colour, int tolerance = 24) {
  int count = 0;
  const int x0 = static_cast<int>(img.width() * r.x0), x1 = static_cast<int>(img.width() * r.x1);
  const int y0 = static_cast<int>(img.height() * r.y0), y1 = static_cast<int>(img.height() * r.y1);
  for (int y = y0; y < y1; ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    for (int x = x0; x < x1; ++x) {
      const QRgb px = line[x];
      if (std::abs(qRed(px) - qRed(colour)) < tolerance && std::abs(qGreen(px) - qGreen(colour)) < tolerance &&
          std::abs(qBlue(px) - qBlue(colour)) < tolerance)
        ++count;
    }
  }
  return count;
}

const QRgb kCritical = qRgb(224, 96, 60);
const QRgb kStubPlayback = qRgb(120, 160, 255);
const QRgb kWarning = qRgb(226, 164, 60);
// rgba(90,169,214,.9) over a dark thumbnail: the relevance badge.
const QRgb kRelevanceBadge = qRgb(84, 154, 196);
// #1A1C1F: skeleton text bars.
const QRgb kRaised = qRgb(26, 28, 31);

const Region kTabStrip{0.01, 0.045, 0.30, 0.10};
const Region kWall{0.18, 0.11, 0.76, 0.99};
const Region kWallTiles{0.18, 0.16, 0.76, 0.99};
const Region kCentre{0.30, 0.25, 0.70, 0.75};
const Region kRulesRail{0.0, 0.20, 0.27, 0.60};
const Region kAlertRows{0.28, 0.24, 0.76, 0.50};
const Region kDetailPlayer{0.77, 0.21, 0.99, 0.40};
const Region kEditorFrame{0.0, 0.42, 0.27, 0.65};
const Region kSearchGrid{0.01, 0.34, 0.74, 0.86};
const Region kSearchPlayer{0.77, 0.34, 0.99, 0.54};
const Region kSearchStats{0.60, 0.20, 0.99, 0.25};
const Region kSearchMessage{0.15, 0.45, 0.60, 0.75};

}

class TestConsoleSmoke : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void monitorShowsLiveFrames();
  void cameraDialogRenders();
  void playbackDialogShowsFrames();
  void monitorFeedDrawsBoxesAndConfirmsAlert();
  void alertLogRendersRulesAndAlerts();
  void alertDetailPlaysEvidence();
  void ruleEditorDrawsOnLiveFrame();
  void searchShowsSkeletonWhileSearching();
  void searchResultsShowThumbnailsAndPlayTheRange();
  void searchEmptyStateShowsOneAction();
  void searchErrorStateShowsTheReason();
  void cleanupTestCase();

private:
  QImage runConsole(const QString& view, QString* problem);
  QString stubOutput();

  QTemporaryDir dir_;
  QProcess stub_;
  int monitorWallBright_ = 0;
  int monitorTabBright_ = 0;
  QString stubLog_;
};

QString TestConsoleSmoke::stubOutput() {
  while (stub_.waitForReadyRead(300)) stubLog_ += QString::fromLocal8Bit(stub_.readAll());
  stubLog_ += QString::fromLocal8Bit(stub_.readAll());
  return stubLog_;
}

void TestConsoleSmoke::initTestCase() {
  QVERIFY(dir_.isValid());
  stub_.setProgram(QStringLiteral(FOVEA_STUB_CORE_BIN));
  stub_.setArguments({QStringLiteral("--data-dir"), dir_.path()});
  stub_.setProcessChannelMode(QProcess::MergedChannels);
  stub_.start();
  QVERIFY2(stub_.waitForStarted(5000), qPrintable(QStringLiteral("stub core did not start: %1").arg(stub_.errorString())));
  QElapsedTimer t;
  t.start();
  while (!QFile::exists(dir_.path() + QStringLiteral("/core.json")) && t.elapsed() < kStubStartMs) QTest::qWait(100);
  QVERIFY2(QFile::exists(dir_.path() + QStringLiteral("/core.json")),
           qPrintable(QStringLiteral("stub core wrote no core.json: %1").arg(QString::fromLocal8Bit(stub_.readAll()))));
  QVERIFY(QFile::exists(dir_.path() + QStringLiteral("/core.token")));
}

QImage TestConsoleSmoke::runConsole(const QString& view, QString* problem) {
  const QString png = dir_.path() + QStringLiteral("/%1.png").arg(view.isEmpty() ? QStringLiteral("monitor") : view);
  QProcess console;
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  const QString platform = env.value(QStringLiteral("FOVEA_SMOKE_PLATFORM"), QStringLiteral("offscreen"));
  env.insert(QStringLiteral("QT_QPA_PLATFORM"), platform);
  env.insert(QStringLiteral("FOVEA_SCREENSHOT"), png);
  env.insert(QStringLiteral("FOVEA_SCREENSHOT_VIEW"), view);
  env.insert(QStringLiteral("FOVEA_SCREENSHOT_DELAY_MS"), QStringLiteral("6000"));
  env.remove(QStringLiteral("FOVEA_CORE_BIN"));
  console.setProcessEnvironment(env);
  console.setProgram(QStringLiteral(FOVEA_CONSOLE_BIN));
  console.setArguments({QStringLiteral("--data-dir"), dir_.path()});
  console.setProcessChannelMode(QProcess::MergedChannels);
  console.start();
  if (!console.waitForStarted(5000)) {
    *problem = QStringLiteral("console did not start: %1").arg(console.errorString());
    return {};
  }
  if (!console.waitForFinished(kConsoleRunMs)) {
    console.kill();
    console.waitForFinished(5000);
    *problem = QStringLiteral("console did not exit after the screenshot: %1").arg(QString::fromLocal8Bit(console.readAll()));
    return {};
  }
  if (console.exitStatus() != QProcess::NormalExit || console.exitCode() != 0) {
    *problem = QStringLiteral("console exited with %1: %2").arg(console.exitCode()).arg(QString::fromLocal8Bit(console.readAll()));
    return {};
  }
  QImage img(png);
  if (img.isNull()) {
    *problem = QStringLiteral("screenshot missing or unreadable: %1").arg(png);
    return {};
  }
  return img.convertToFormat(QImage::Format_ARGB32);
}

void TestConsoleSmoke::monitorShowsLiveFrames() {
  QString problem;
  const QImage img = runConsole({}, &problem);
  if (img.isNull()) {
    if (strict()) QFAIL(qPrintable(problem));
    QSKIP(qPrintable(QStringLiteral("headless console run unavailable: %1").arg(problem)));
  }
  QVERIFY2(img.width() >= 1440 && img.height() >= 900, "window smaller than the design minimum");
  monitorWallBright_ = brightPixels(img, kWall);
  monitorTabBright_ = brightPixels(img, kTabStrip);
  QVERIFY2(monitorTabBright_ > 100, "tab bar labels are missing");
  QVERIFY2(monitorWallBright_ > 4000,
           qPrintable(QStringLiteral("wall shows no stub pattern (bright pixels: %1)").arg(monitorWallBright_)));
}

void TestConsoleSmoke::cameraDialogRenders() {
  QString problem;
  const QImage img = runConsole(QStringLiteral("camera-dialog"), &problem);
  if (img.isNull()) {
    if (strict()) QFAIL(qPrintable(problem));
    QSKIP(qPrintable(QStringLiteral("headless console run unavailable: %1").arg(problem)));
  }
  const int tab = brightPixels(img, kTabStrip);
  QVERIFY2(tab < monitorTabBright_ / 4, qPrintable(QStringLiteral("backdrop scrim missing (tab strip bright: %1)").arg(tab)));
  const int centre = brightPixels(img, kCentre);
  QVERIFY2(centre > 300, qPrintable(QStringLiteral("dialog content missing (centre bright: %1)").arg(centre)));
}

void TestConsoleSmoke::playbackDialogShowsFrames() {
  QString problem;
  const QImage img = runConsole(QStringLiteral("playback"), &problem);
  if (img.isNull()) {
    if (strict()) QFAIL(qPrintable(problem));
    QSKIP(qPrintable(QStringLiteral("headless console run unavailable: %1").arg(problem)));
  }
  const int tab = brightPixels(img, kTabStrip);
  QVERIFY2(tab < monitorTabBright_ / 4, qPrintable(QStringLiteral("backdrop scrim missing (tab strip bright: %1)").arg(tab)));
  const int centre = brightPixels(img, kCentre);
  QVERIFY2(centre > 3000, qPrintable(QStringLiteral("playback pattern missing (centre bright: %1)").arg(centre)));
}

void TestConsoleSmoke::monitorFeedDrawsBoxesAndConfirmsAlert() {
  QString problem;
  const QImage img = runConsole(QStringLiteral("monitor-feed"), &problem);
  if (img.isNull()) {
    if (strict()) QFAIL(qPrintable(problem));
    QSKIP(qPrintable(QStringLiteral("headless console run unavailable: %1").arg(problem)));
  }
  const int boxes = colourPixels(img, kWallTiles, kCritical);
  qInfo("critical pixels on the wall: %d", boxes);
  QVERIFY2(boxes > 150, qPrintable(QStringLiteral("no detection boxes on the wall (critical pixels: %1)").arg(boxes)));
  const QString log = stubOutput();
  QVERIFY2(log.contains(QStringLiteral("STUB raised live alert")), qPrintable(log));
  QVERIFY2(log.contains(QStringLiteral("STUB delivered console Person in Gate apron")),
           "the live alert was not confirmed as shown on the console");
  qInfo("sound delivery confirmed: %s", log.contains(QStringLiteral("STUB delivered sound")) ? "yes" : "no");
}

void TestConsoleSmoke::alertLogRendersRulesAndAlerts() {
  QString problem;
  const QImage img = runConsole(QStringLiteral("alerts"), &problem);
  if (img.isNull()) {
    if (strict()) QFAIL(qPrintable(problem));
    QSKIP(qPrintable(QStringLiteral("headless console run unavailable: %1").arg(problem)));
  }
  const int rules = brightPixels(img, kRulesRail);
  const int rows = brightPixels(img, kAlertRows);
  qInfo("bright pixels: rules rail %d, alert rows %d", rules, rows);
  QVERIFY2(rules > 400, qPrintable(QStringLiteral("rule cards missing (bright: %1)").arg(rules)));
  QVERIFY2(rows > 400, qPrintable(QStringLiteral("alert rows missing (bright: %1)").arg(rows)));
}

void TestConsoleSmoke::alertDetailPlaysEvidence() {
  QString problem;
  const QImage img = runConsole(QStringLiteral("alert-detail"), &problem);
  if (img.isNull()) {
    if (strict()) QFAIL(qPrintable(problem));
    QSKIP(qPrintable(QStringLiteral("headless console run unavailable: %1").arg(problem)));
  }
  const int player = colourPixels(img, kDetailPlayer, kStubPlayback);
  qInfo("playback pattern pixels in the evidence player: %d", player);
  QVERIFY2(player > 300, qPrintable(QStringLiteral("evidence clip not playing (pattern pixels: %1)").arg(player)));
}

void TestConsoleSmoke::ruleEditorDrawsOnLiveFrame() {
  QString problem;
  const QImage img = runConsole(QStringLiteral("rule-editor"), &problem);
  if (img.isNull()) {
    if (strict()) QFAIL(qPrintable(problem));
    QSKIP(qPrintable(QStringLiteral("headless console run unavailable: %1").arg(problem)));
  }
  const int frame = brightPixels(img, kEditorFrame);
  qInfo("bright pixels in the zone editor: %d", frame);
  QVERIFY2(frame > 3000, qPrintable(QStringLiteral("zone editor shows no live frame (bright: %1)").arg(frame)));
}

void TestConsoleSmoke::searchShowsSkeletonWhileSearching() {
  QString problem;
  const QImage img = runConsole(QStringLiteral("search"), &problem);
  if (img.isNull()) {
    if (strict()) QFAIL(qPrintable(problem));
    QSKIP(qPrintable(QStringLiteral("headless console run unavailable: %1").arg(problem)));
  }
  const int bright = brightPixels(img, kSearchGrid);
  const int bars = colourPixels(img, kSearchGrid, kRaised, 3);
  qInfo("search skeleton: bright %d, skeleton bar pixels %d", bright, bars);
  QVERIFY2(bright < 50, qPrintable(QStringLiteral("results drawn while searching (bright: %1)").arg(bright)));
  QVERIFY2(bars > 8000, qPrintable(QStringLiteral("skeleton cards missing (bar pixels: %1)").arg(bars)));
  QVERIFY2(stubOutput().contains(QStringLiteral("STUB search \"person walking through the lobby [slow]\" cameras=2")),
           "the query did not reach the service with every camera");
}

void TestConsoleSmoke::searchResultsShowThumbnailsAndPlayTheRange() {
  QString problem;
  const QImage img = runConsole(QStringLiteral("search-results"), &problem);
  if (img.isNull()) {
    if (strict()) QFAIL(qPrintable(problem));
    QSKIP(qPrintable(QStringLiteral("headless console run unavailable: %1").arg(problem)));
  }
  const int thumbnails = brightPixels(img, kSearchGrid);
  const int badges = colourPixels(img, kSearchGrid, kRelevanceBadge);
  const int player = colourPixels(img, kSearchPlayer, kStubPlayback);
  const int coverage = colourPixels(img, kSearchStats, kWarning);
  qInfo("search results: thumbnail bright %d, badge %d, player pattern %d, coverage warning %d", thumbnails, badges, player,
        coverage);
  QVERIFY2(thumbnails > 8000, qPrintable(QStringLiteral("result thumbnails missing (bright: %1)").arg(thumbnails)));
  QVERIFY2(badges > 3000, qPrintable(QStringLiteral("relevance badges missing (pixels: %1)").arg(badges)));
  QVERIFY2(player > 300, qPrintable(QStringLiteral("selected result not playing (pattern pixels: %1)").arg(player)));
  QVERIFY2(coverage > 30, qPrintable(QStringLiteral("coverage below 100%% not in warning colour (pixels: %1)").arg(coverage)));
}

void TestConsoleSmoke::searchEmptyStateShowsOneAction() {
  QString problem;
  const QImage img = runConsole(QStringLiteral("search-empty"), &problem);
  if (img.isNull()) {
    if (strict()) QFAIL(qPrintable(problem));
    QSKIP(qPrintable(QStringLiteral("headless console run unavailable: %1").arg(problem)));
  }
  const int action = brightPixels(img, kSearchMessage);
  const int badges = colourPixels(img, kSearchGrid, kRelevanceBadge);
  qInfo("search empty: action bright %d, badge %d", action, badges);
  QVERIFY2(action > 80, qPrintable(QStringLiteral("empty state action missing (bright: %1)").arg(action)));
  QVERIFY2(badges < 50, qPrintable(QStringLiteral("result cards drawn for an empty result (badge: %1)").arg(badges)));
}

void TestConsoleSmoke::searchErrorStateShowsTheReason() {
  QString problem;
  const QImage img = runConsole(QStringLiteral("search-error"), &problem);
  if (img.isNull()) {
    if (strict()) QFAIL(qPrintable(problem));
    QSKIP(qPrintable(QStringLiteral("headless console run unavailable: %1").arg(problem)));
  }
  const int reason = colourPixels(img, kSearchMessage, kCritical);
  const int retry = brightPixels(img, kSearchMessage);
  qInfo("search error: reason pixels %d, retry bright %d", reason, retry);
  QVERIFY2(reason > 100, qPrintable(QStringLiteral("error reason missing (critical pixels: %1)").arg(reason)));
  QVERIFY2(retry > 80, qPrintable(QStringLiteral("retry action missing (bright: %1)").arg(retry)));
}

void TestConsoleSmoke::cleanupTestCase() {
  if (stub_.state() == QProcess::NotRunning) return;
  stub_.terminate();
  if (!stub_.waitForFinished(5000)) {
    stub_.kill();
    stub_.waitForFinished(2000);
  }
}

QTEST_GUILESS_MAIN(TestConsoleSmoke)
#include "console_smoke.moc"

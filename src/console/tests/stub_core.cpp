// STUB CORE - test fixture, not real data.
//
// Fakes enough of docs/API.md and the M3 additions (docs/M3_DESIGN.md) for
// console UI verification: two cameras with a moving test pattern and a drawn
// "person" figure in their frame rings, canned segments and gaps, playback
// channels that render a pattern with a running clock, TEST FIXTURE zones,
// rules, events with evaluations and evidence in each state, detections that
// follow the drawn figure, and one live alert per console session raised
// shortly after that console first asks for pending alerts. Nothing here
// touches a real stream, file, model or database. POST /v1/cameras/test fails
// for any main_url containing "fail" (the second seeded camera).
#include "fovea/Api.h"
#include "fovea/Clock.h"
#include "fovea/FrameRing.h"
#include "fovea/Token.h"
#include <QCommandLineParser>
#include <QBuffer>
#include <QDir>
#include <QFuture>
#include <QGuiApplication>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPromise>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QTcpServer>
#include <QTextStream>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <csignal>
#include <cstdio>
#include <algorithm>
#include <deque>
#include <map>
#include <memory>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using Status = QHttpServerResponse::StatusCode;

constexpr uint32_t kRingSlots = 3;
constexpr uint32_t kRingMaxWidth = 1280;
constexpr uint32_t kRingMaxHeight = 720;
constexpr int kFrameWidth = 640;
constexpr int kFrameHeight = 360;
constexpr int kFrameIntervalMs = 40;
constexpr int64_t kNsPerMs = 1'000'000;
constexpr int64_t kNsPerSecond = 1'000'000'000;
constexpr int64_t kSecondMs = 1000;
// Detections trail the displayed frame the way a real detector lane does.
constexpr int kDetectionLagFrames = 4;
constexpr int kDetectionHistory = 32;
constexpr int kLiveAlertDelayMs = 1500;

struct FigureSample {
  int64_t ptsNs = 0;
  int64_t utcMs = 0;
  uint64_t frame = 0;
  QRectF box;
};

struct StubCamera {
  fovea::Camera camera;
  fovea::CameraStatus status;
  std::unique_ptr<fovea::FrameRingWriter> ring;
  QColor tint;
  QVector<fovea::RecordingSegment> segments;
  QVector<fovea::ReceiveGap> gaps;
  std::deque<FigureSample> figures;
};

struct StubEvent {
  QJsonObject event;
  QJsonArray evaluations;
  QByteArray thumbnailJpeg;
};

struct StubPlayback {
  fovea::PlaybackState state;
  std::unique_ptr<fovea::FrameRingWriter> ring;
  QString cameraCode;
};

QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

fovea::RingRef ringRef(const fovea::FrameRingWriter& w) {
  fovea::RingRef r;
  r.name = w.info().name;
  r.slotCount = w.info().slotCount;
  r.slotBytes = w.info().slotBytes;
  r.maxWidth = w.info().maxWidth;
  r.maxHeight = w.info().maxHeight;
  return r;
}

QHttpServerResponse json(const QJsonObject& o, Status s = Status::Ok) { return QHttpServerResponse(o, s); }
QHttpServerResponse json(const QJsonArray& a) { return QHttpServerResponse(a, Status::Ok); }
QHttpServerResponse unauthorized() {
  return QHttpServerResponse(fovea::errorJson(QStringLiteral("unauthorized"), QStringLiteral("missing or invalid token")),
                             Status::Unauthorized);
}
double num(int64_t v) { return static_cast<double>(v); }

// One line on stdout, flushed at once: the smoke test reads it while the stub keeps running.
void report(const QString& line) {
  std::fputs(qPrintable(line + QLatin1Char('\n')), stdout);
  std::fflush(stdout);
}

// The drawn figure walks across the frame and back; bbox normalized to the frame.
QRectF figureBox(const QString& code, uint64_t frame) {
  const double phase = static_cast<double>(frame % 400) / 400.0;
  const double walk = phase < 0.5 ? phase * 2.0 : (1.0 - phase) * 2.0;
  if (code == QLatin1StringView("CAM-01")) return QRectF(0.22 + 0.38 * walk, 0.36, 0.10, 0.46);
  return QRectF(0.52 + 0.20 * walk, 0.30, 0.09, 0.42);
}
QHttpServerResponse fail(Status s, const QString& code, const QString& message) {
  return QHttpServerResponse(fovea::errorJson(code, message), s);
}

std::optional<QJsonObject> body(const QHttpServerRequest& req) {
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(req.body(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
  return doc.object();
}

class StubCore : public QObject {
public:
  explicit StubCore(QString dataDir) : dataDir_(std::move(dataDir)) {}

  bool start() {
    if (!QDir().mkpath(dataDir_)) return false;
    token_ = fovea::loadOrCreateToken(dataDir_ + "/core.token");
    if (token_.isEmpty()) return false;
    tcp_ = new QTcpServer(this);
    if (!tcp_->listen(QHostAddress::LocalHost, 0)) return false;
    registerRoutes();
    if (!server_.bind(tcp_)) return false;
    port_ = tcp_->serverPort();
    seedCameras();
    seedRules();
    seedEvents();
    frameTimer_.setTimerType(Qt::PreciseTimer);
    frameTimer_.setInterval(kFrameIntervalMs);
    connect(&frameTimer_, &QTimer::timeout, this, [this] { tick(); });
    frameTimer_.start();
    return writeDiscovery();
  }

  quint16 port() const { return port_; }

private:
  bool writeDiscovery() {
    QSaveFile f(dataDir_ + "/core.json");
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QJsonObject info{{"port", port_}, {"pid", static_cast<double>(QCoreApplication::applicationPid())},
                           {"started_utc_ms", static_cast<double>(fovea::utcNowMs())}, {"version", "stub"}};
    f.write(QJsonDocument(info).toJson(QJsonDocument::Compact));
    return f.commit();
  }

  bool authorized(const QHttpServerRequest& req) const {
    const QByteArray auth = req.value("Authorization");
    return auth.startsWith("Bearer ") && fovea::tokenEquals(QString::fromLatin1(auth.mid(7)).trimmed(), token_);
  }

  void seedCameras() {
    const int64_t now = fovea::utcNowMs();
    addCamera(QStringLiteral("CAM-01"), QStringLiteral("North Gate"), QStringLiteral("Harbour District"), QColor(255, 176, 64), now);
    addCamera(QStringLiteral("CAM-02"), QStringLiteral("Terminal Lobby"), QStringLiteral("Terminal A"), QColor(96, 224, 168), now);
  }

  void addCamera(const QString& code, const QString& name, const QString& group, const QColor& tint, int64_t now) {
    auto cam = std::make_unique<StubCamera>();
    cam->camera.id = uuid();
    cam->camera.code = code;
    cam->camera.name = name;
    cam->camera.groupName = group;
    const QString host = order_.isEmpty() ? QStringLiteral("stub.invalid") : QStringLiteral("stub-fail.invalid");
    cam->camera.mainUrl = QStringLiteral("rtsp://%1/%2/main").arg(host, code.toLower());
    cam->camera.subUrl = QStringLiteral("rtsp://%1/%2/sub").arg(host, code.toLower());
    cam->camera.createdUtcMs = now;
    cam->camera.updatedUtcMs = now;
    cam->tint = tint;
    cam->ring = fovea::FrameRingWriter::create(fovea::makeRingName(QStringLiteral("stub-cam-") + cam->camera.id),
                                               kRingSlots, kRingMaxWidth, kRingMaxHeight);
    if (!cam->ring) {
      QTextStream(stderr) << "cannot create frame ring for " << code << "\n";
      return;
    }
    fovea::CameraStatus& s = cam->status;
    s.cameraId = cam->camera.id;
    s.state = QStringLiteral("online");
    s.sessionId = uuid();
    s.sinceUtcMs = now - 3'600'000;
    s.stale = false;
    s.codec = QStringLiteral("video/x-h265");
    s.width = kFrameWidth;
    s.height = kFrameHeight;
    s.fpsNew = 25;
    cam->camera.analyticsEnabled = true;
    s.recording = QStringLiteral("recording");
    s.frameRing = ringRef(*cam->ring);
    seedRecordings(*cam, now);
    order_.push_back(cam->camera.id);
    const QString id = cam->camera.id;
    cameras_.emplace(id, std::move(cam));
  }

  void seedRecordings(StubCamera& cam, int64_t now) {
    auto segment = [&](int n, const QString& state, int64_t startOffsetMs, int64_t endOffsetMs) {
      fovea::RecordingSegment s;
      s.id = QStringLiteral("seg-%1-%2").arg(cam.camera.code.toLower()).arg(n);
      s.cameraId = cam.camera.id;
      s.sessionId = cam.status.sessionId;
      s.path = QStringLiteral("%1/recordings/%2/%3.mkv").arg(dataDir_, cam.camera.code, s.id);
      s.state = state;
      s.startUtcMs = now - startOffsetMs;
      s.endUtcMs = endOffsetMs < 0 ? 0 : now - endOffsetMs;
      s.bytes = state == QLatin1StringView("recording") ? 0 : 12'582'912;
      s.createdUtcMs = s.startUtcMs;
      s.finalizedUtcMs = state == QLatin1StringView("finalized") ? s.endUtcMs : 0;
      cam.segments.push_back(s);
    };
    segment(1, QStringLiteral("recording"), 45'000, -1);
    segment(2, QStringLiteral("finalized"), 105'000, 45'000);
    segment(3, QStringLiteral("finalized"), 165'000, 105'000);
    segment(4, QStringLiteral("damaged"), 230'000, 195'000);
    segment(5, QStringLiteral("finalized"), 7'200'000, 7'140'000);
    cam.status.currentSegmentId = cam.segments.first().id;
    auto gap = [&](int n, int64_t fromOffsetMs, int64_t toOffsetMs, const QString& reason) {
      fovea::ReceiveGap g;
      g.id = QStringLiteral("gap-%1-%2").arg(cam.camera.code.toLower()).arg(n);
      g.cameraId = cam.camera.id;
      g.sessionId = cam.status.sessionId;
      g.fromUtcMs = now - fromOffsetMs;
      g.toUtcMs = now - toOffsetMs;
      g.reason = reason;
      cam.gaps.push_back(g);
    };
    gap(1, 195'000, 165'000, QStringLiteral("no packets for 30 s"));
    gap(2, 10'800'000, 10'792'000, QStringLiteral("reconnect"));
  }

  StubCamera* cameraByCode(const QString& code) const {
    for (const auto& entry : cameras_)
      if (entry.second->camera.code == code) return entry.second.get();
    return nullptr;
  }

  QJsonObject zoneJson(const QString& id, const QString& cameraId, const QString& name, const QJsonArray& points,
                       int64_t now) {
    return {{"id", id}, {"camera_id", cameraId}, {"name", name}, {"revision", 1}, {"points", points},
            {"anchor", "foot"}, {"ref_width", kFrameWidth}, {"ref_height", kFrameHeight}, {"created_utc_ms", num(now)}};
  }

  static QJsonArray polygon(std::initializer_list<std::pair<double, double>> points) {
    QJsonArray out;
    for (const auto& [x, y] : points) out.push_back(QJsonArray{x, y});
    return out;
  }

  QJsonObject ruleJson(const QString& id, const QString& name, const QString& cameraId, const QString& zoneId,
                       bool enabled, int days, int startMinute, int endMinute, const QString& tz, double confidence,
                       int dwellSeconds, const QString& severity, bool sound, bool pop, int64_t now) {
    return {{"id", id}, {"name", name}, {"revision", 1}, {"enabled", enabled}, {"camera_id", cameraId},
            {"zone_id", zoneId}, {"zone_revision", 1},
            {"schedule", QJsonArray{QJsonObject{{"days", days}, {"start_minute", startMinute}, {"end_minute", endMinute}}}},
            {"time_zone", tz}, {"target_class", "person"}, {"min_confidence", confidence},
            {"dwell_ns", num(dwellSeconds * kNsPerSecond)}, {"max_observation_gap_ns", num(1'500'000'000)},
            {"clear_after_ns", num(5 * kNsPerSecond)}, {"rearm_ns", num(30 * kNsPerSecond)},
            {"result_ttl_ns", num(5 * kNsPerSecond)}, {"evidence_pre_ns", num(10 * kNsPerSecond)},
            {"evidence_post_ns", num(10 * kNsPerSecond)}, {"vlm_role", "none"}, {"severity", severity},
            {"actions", QJsonObject{{"sound", sound}, {"pop_to_main_view", pop}}}, {"created_utc_ms", num(now)},
            {"runtime", QJsonArray{QJsonObject{{"camera_id", cameraId}, {"condition", "inactive"}, {"quality", "known"}}}}};
  }

  void seedRules() {
    const int64_t now = fovea::utcNowMs();
    const StubCamera* gate = cameraByCode(QStringLiteral("CAM-01"));
    const StubCamera* lobby = cameraByCode(QStringLiteral("CAM-02"));
    if (!gate || !lobby) return;
    addZone(zoneJson(uuid(), gate->camera.id, QStringLiteral("Gate apron (test fixture)"),
                     polygon({{0.18, 0.30}, {0.66, 0.26}, {0.74, 0.88}, {0.12, 0.92}}), now));
    addZone(zoneJson(uuid(), lobby->camera.id, QStringLiteral("Turnstiles (test fixture)"),
                     polygon({{0.44, 0.22}, {0.88, 0.22}, {0.88, 0.80}, {0.44, 0.80}}), now));
    const QString gateZone = zoneOrder_[0];
    const QString lobbyZone = zoneOrder_[1];
    addRule(ruleJson(uuid(), QStringLiteral("Gate apron dwell"), gate->camera.id, gateZone, true, 0x1f, 7 * 60, 19 * 60,
                     QStringLiteral("Asia/Seoul"), 0.5, 10, QStringLiteral("critical"), true, true, now));
    addRule(ruleJson(uuid(), QStringLiteral("Turnstile loitering"), lobby->camera.id, lobbyZone, true, 0x7f, 0, 24 * 60,
                     QStringLiteral("UTC"), 0.4, 30, QStringLiteral("review"), true, false, now));
    addRule(ruleJson(uuid(), QStringLiteral("After-hours gate"), gate->camera.id, gateZone, false, 0x7f, 22 * 60, 6 * 60,
                     QStringLiteral("Asia/Seoul"), 0.6, 5, QStringLiteral("info"), false, false, now));
  }

  void addZone(const QJsonObject& zone) {
    zoneOrder_.push_back(zone.value("id").toString());
    zones_[zoneOrder_.last()] = zone;
  }

  void addRule(const QJsonObject& rule) {
    ruleOrder_.push_back(rule.value("id").toString());
    rules_[ruleOrder_.last()] = rule;
  }

  QJsonObject evaluation(const QString& eventId, const QJsonObject& rule, int64_t utcMs, const QString& transition,
                         const QString& before, const QString& after, double dwellSeconds, const QStringList& tracks,
                         const QString& note = {}) const {
    return {{"id", uuid()}, {"rule_id", rule.value("id")}, {"rule_revision", rule.value("revision")},
            {"camera_id", rule.value("camera_id")}, {"utc_ms", num(utcMs)}, {"before", before}, {"after", after},
            {"quality", "known"}, {"transition", transition}, {"event_id", eventId},
            {"track_ids", QJsonArray::fromStringList(tracks)}, {"dwell_ns", dwellSeconds * 1e9}, {"note", note}};
  }

  static QJsonObject delivery(const QString& eventId, const QString& channel, const QString& state, int attempts,
                              int64_t created, int64_t delivered, const QString& error = {}) {
    return {{"id", uuid()}, {"event_id", eventId}, {"channel", channel}, {"state", state}, {"attempts", attempts},
            {"last_error", error}, {"created_utc_ms", num(created)}, {"delivered_utc_ms", num(delivered)}};
  }

  QByteArray thumbnail(const StubCamera& cam, int64_t utcMs) {
    QImage image(kFrameWidth, kFrameHeight, QImage::Format_RGB32);
    image.fill(QColor(24, 28, 34));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(cam.tint, 4));
    p.drawRect(QRect(2, 2, kFrameWidth - 5, kFrameHeight - 5));
    const QRectF box = figureBox(cam.camera.code, 100);
    paintFigure(p, box);
    QFont big;
    big.setPixelSize(24);
    big.setBold(true);
    p.setFont(big);
    p.setPen(Qt::white);
    p.drawText(QRect(20, 16, kFrameWidth - 40, 34), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("TEST FIXTURE · %1").arg(cam.camera.code));
    QFont small;
    small.setPixelSize(16);
    p.setFont(small);
    p.drawText(QRect(20, kFrameHeight - 46, kFrameWidth - 40, 26), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("evidence thumbnail · %1").arg(QDateTime::fromMSecsSinceEpoch(utcMs).toString(QStringLiteral("HH:mm:ss"))));
    p.end();
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 85);
    return jpeg;
  }

  struct EventSpec {
    QString ruleId;
    QString title;
    QString detail;
    int64_t openedAgoMs = 0;
    int64_t clearedAgoMs = 0;
    QString condition;
    QString operatorState;
    QString reviewLabel;
    QString reviewNote;
    QString evidenceState;
    QString evidenceReason;
    QString deliveryState;
  };

  QString addEvent(const EventSpec& spec, int64_t now) {
    const QJsonObject rule = rules_.at(spec.ruleId);
    const StubCamera* cam = camera(rule.value("camera_id").toString());
    const QString id = uuid();
    const int64_t opened = now - spec.openedAgoMs;
    const double dwell = rule.value("dwell_ns").toDouble() / 1e9;
    QJsonObject e{{"id", id}, {"rule_id", spec.ruleId}, {"rule_revision", rule.value("revision")},
                  {"camera_id", rule.value("camera_id")}, {"session_id", cam->status.sessionId},
                  {"severity", rule.value("severity")}, {"condition", spec.condition},
                  {"operator_state", spec.operatorState}, {"opened_utc_ms", num(opened)},
                  {"trigger_pts_ns", num(opened * kNsPerMs)}, {"cleared_utc_ms", num(spec.clearedAgoMs > 0 ? now - spec.clearedAgoMs : 0)},
                  {"title", spec.title}, {"detail", spec.detail}, {"late", 0}};
    if (!spec.reviewLabel.isEmpty())
      e.insert("review", QJsonObject{{"label", spec.reviewLabel}, {"note", spec.reviewNote}, {"operator", "console"},
                                     {"utc_ms", num(opened + 60 * kSecondMs)}});
    const QString evidenceId = uuid();
    e.insert("evidence", QJsonObject{{"id", evidenceId}, {"event_id", id}, {"camera_id", rule.value("camera_id")},
                                     {"from_utc_ms", num(opened - 10 * kSecondMs)}, {"to_utc_ms", num(opened + 10 * kSecondMs)},
                                     {"state", spec.evidenceState}, {"reason", spec.evidenceReason},
                                     {"updated_utc_ms", num(now)}});
    const bool delivered = spec.deliveryState == QLatin1StringView("delivered");
    const int attempts = spec.deliveryState == QLatin1StringView("failed") ? 3 : delivered ? 1 : 0;
    const QString error = spec.deliveryState == QLatin1StringView("failed") ? QStringLiteral("no console connected") : QString();
    QJsonArray deliveries{delivery(id, QStringLiteral("console"), spec.deliveryState, attempts, opened, delivered ? opened + 800 : 0, error)};
    if (rule.value("actions").toObject().value("sound").toBool())
      deliveries.push_back(delivery(id, QStringLiteral("sound"), spec.deliveryState, attempts, opened, delivered ? opened + 900 : 0, error));
    e.insert("deliveries", deliveries);

    auto ev = std::make_unique<StubEvent>();
    const QStringList track{QString::number(3 + static_cast<int>(events_.size()) * 2)};
    ev->evaluations.push_back(evaluation(id, rule, opened - static_cast<int64_t>(dwell * 1000) - 400, QStringLiteral("became_pending"),
                                         QStringLiteral("inactive"), QStringLiteral("pending"), 0, track));
    ev->evaluations.push_back(evaluation(id, rule, opened, QStringLiteral("triggered"), QStringLiteral("pending"),
                                         QStringLiteral("active"), dwell + 0.4, track));
    if (spec.condition != QLatin1StringView("active")) {
      const int64_t clearing = spec.clearedAgoMs > 0 ? now - spec.clearedAgoMs - 5 * kSecondMs : now - 12 * kSecondMs;
      ev->evaluations.push_back(evaluation(id, rule, clearing, QStringLiteral("became_clearing"), QStringLiteral("active"),
                                           QStringLiteral("clearing"), 0, {}));
    }
    if (spec.condition == QLatin1StringView("cleared"))
      ev->evaluations.push_back(evaluation(id, rule, now - spec.clearedAgoMs, QStringLiteral("cleared"), QStringLiteral("clearing"),
                                           QStringLiteral("inactive"), 0, {}));
    if (spec.evidenceState != QLatin1StringView("deleted")) ev->thumbnailJpeg = thumbnail(*cam, opened);
    ev->event = e;
    events_.push_back(std::move(ev));
    return id;
  }

  void seedEvents() {
    if (ruleOrder_.size() < 2) return;
    const int64_t now = fovea::utcNowMs();
    const QString gate = ruleOrder_[0];
    const QString lobby = ruleOrder_[1];
    addEvent({gate, QStringLiteral("Person in Gate apron for 10 s"), QStringLiteral("TEST FIXTURE · one person stayed in the zone 10.4 s"),
              80 * kSecondMs, 0, QStringLiteral("active"), QStringLiteral("new"), {}, {}, QStringLiteral("available"), {},
              QStringLiteral("delivered")}, now);
    addEvent({lobby, QStringLiteral("Person in Turnstiles for 30 s"), QStringLiteral("TEST FIXTURE · one person stayed in the zone 30.4 s"),
              40 * kSecondMs, 0, QStringLiteral("clearing"), QStringLiteral("acknowledged"), {}, {}, QStringLiteral("partial"),
              QStringLiteral("window ends inside the segment still recording"), QStringLiteral("delivered")}, now);
    addEvent({lobby, QStringLiteral("Person in Turnstiles for 30 s"), QStringLiteral("TEST FIXTURE · delivery failed while no console was connected"),
              180 * kSecondMs, 150 * kSecondMs, QStringLiteral("cleared"), QStringLiteral("new"), {}, {}, QStringLiteral("partial"),
              QStringLiteral("a receive gap intersects the window"), QStringLiteral("failed")}, now);
    addEvent({lobby, QStringLiteral("Person in Turnstiles for 30 s"), QStringLiteral("TEST FIXTURE · confirmed by the operator"),
              7175 * kSecondMs, 7150 * kSecondMs, QStringLiteral("cleared"), QStringLiteral("resolved"), QStringLiteral("confirmed"),
              QStringLiteral("visitor waited for a guard"), QStringLiteral("available"), {}, QStringLiteral("delivered")}, now);
    addEvent({gate, QStringLiteral("Person in Gate apron for 10 s"), QStringLiteral("TEST FIXTURE · shadow of the barrier arm"),
              7180 * kSecondMs, 7160 * kSecondMs, QStringLiteral("cleared"), QStringLiteral("resolved"), QStringLiteral("false_alarm"),
              QStringLiteral("shadow of the barrier arm"), QStringLiteral("deleted"), QStringLiteral("segment deleted by retention"),
              QStringLiteral("delivered")}, now);
  }

  StubEvent* findEvent(const QString& id) const {
    for (const auto& ev : events_)
      if (ev->event.value("id").toString() == id) return ev.get();
    return nullptr;
  }

  void maybeRaiseLiveAlert(const QString& consoleId) {
    if (consoleId.isEmpty() || ruleOrder_.isEmpty()) return;
    const int64_t now = fovea::utcNowMs();
    const auto seen = consolesSeen_.find(consoleId);
    if (seen == consolesSeen_.end()) {
      consolesSeen_[consoleId] = now;
      return;
    }
    if (seen->second == 0 || now - seen->second < kLiveAlertDelayMs) return;
    seen->second = 0;
    addEvent({ruleOrder_[0], QStringLiteral("Person in Gate apron for 10 s"),
              QStringLiteral("TEST FIXTURE · live alert raised for this console session"), 0, 0, QStringLiteral("active"),
              QStringLiteral("new"), {}, {}, QStringLiteral("pending"), {}, QStringLiteral("pending")}, now);
    report(QStringLiteral("STUB raised live alert for console %1").arg(consoleId));
  }

  StubCamera* camera(const QString& id) const {
    const auto it = cameras_.find(id);
    return it == cameras_.end() ? nullptr : it->second.get();
  }

  StubPlayback* playback(const QString& id) const {
    const auto it = playbacks_.find(id);
    return it == playbacks_.end() ? nullptr : it->second.get();
  }

  QJsonObject cameraJson(const StubCamera& cam) const {
    QJsonObject o = cam.camera.toJson();
    o.insert("status", cam.status.toJson());
    return o;
  }

  const fovea::RecordingSegment* findSegment(const QString& id, const StubCamera** owner) const {
    for (const QString& camId : order_) {
      const StubCamera* cam = camera(camId);
      for (const fovea::RecordingSegment& s : cam->segments) {
        if (s.id == id) {
          *owner = cam;
          return &s;
        }
      }
    }
    return nullptr;
  }

  QHttpServerResponse playbackJson(const StubPlayback& pb) { return json(pb.state.toJson()); }

  void registerRoutes() {
    using Method = QHttpServerRequest::Method;
    auto guard = [this](const QHttpServerRequest& req, std::optional<QHttpServerResponse>& out) {
      if (!authorized(req)) out.emplace(fail(Status::Unauthorized, "unauthorized", "missing or invalid token"));
    };

    server_.route("/v1/health", Method::Get, [this, guard](const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      return json(QJsonObject{{"status", "ok"}, {"version", "stub"},
                              {"uptime_ms", static_cast<double>(fovea::utcNowMs() - startedUtcMs_)}, {"data_dir", dataDir_}});
    });

    server_.route("/v1/cameras", Method::Get, [this, guard](const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      QJsonArray arr;
      for (const QString& id : order_) arr.push_back(cameraJson(*camera(id)));
      return json(arr);
    });

    server_.route("/v1/cameras", Method::Post, [this, guard](const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      const auto b = body(req);
      if (!b) return fail(Status::BadRequest, "bad_json", "body must be a JSON object");
      fovea::Camera c = fovea::Camera::fromJson(*b);
      if (c.name.isEmpty() || c.mainUrl.isEmpty()) return fail(Status::BadRequest, "invalid_camera", "name and main_url are required");
      const int64_t now = fovea::utcNowMs();
      addCamera(c.code.isEmpty() ? QStringLiteral("CAM-%1").arg(order_.size() + 1, 2, 10, QLatin1Char('0')) : c.code,
                c.name, c.groupName, QColor(120, 160, 255), now);
      StubCamera& created = *camera(order_.last());
      const QString id = created.camera.id;
      created.camera = c;
      created.camera.id = id;
      created.camera.createdUtcMs = now;
      created.camera.updatedUtcMs = now;
      return json(cameraJson(created), Status::Created);
    });

    server_.route("/v1/cameras/test", Method::Post, [guard](const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      const auto b = body(req);
      if (!b) return fail(Status::BadRequest, "bad_json", "body must be a JSON object");
      fovea::ConnectionTest t;
      if (b->value("main_url").toString().contains(QLatin1StringView("fail"))) {
        t.ok = false;
        t.error = QStringLiteral("RTSP DESCRIBE timed out after 8 s (stub)");
      } else {
        t.ok = true;
        t.handshakeMs = 184;
        t.codec = QStringLiteral("video/x-h265");
        t.width = 1920;
        t.height = 1080;
        t.fps = 25;
        t.bitrateKbps = 4200;
      }
      return json(t.toJson());
    });

    server_.route("/v1/cameras/<arg>", Method::Get, [this, guard](const QString& id, const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      const StubCamera* cam = camera(id);
      if (!cam) return fail(Status::NotFound, "not_found", "no such camera");
      return json(cameraJson(*cam));
    });

    server_.route("/v1/cameras/<arg>", Method::Put, [this, guard](const QString& id, const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      StubCamera* cam = camera(id);
      if (!cam) return fail(Status::NotFound, "not_found", "no such camera");
      const auto b = body(req);
      if (!b) return fail(Status::BadRequest, "bad_json", "body must be a JSON object");
      QJsonObject merged = cam->camera.toJson();
      for (auto it = b->begin(); it != b->end(); ++it) merged.insert(it.key(), it.value());
      cam->camera = fovea::Camera::fromJson(merged);
      cam->camera.id = id;
      cam->camera.updatedUtcMs = fovea::utcNowMs();
      if (cam->camera.name.isEmpty() || cam->camera.mainUrl.isEmpty()) return fail(Status::BadRequest, "invalid_camera", "name and main_url are required");
      return json(cameraJson(*cam));
    });

    server_.route("/v1/cameras/<arg>", Method::Delete, [this, guard](const QString& id, const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      if (cameras_.erase(id) == 0) return fail(Status::NotFound, "not_found", "no such camera");
      order_.removeAll(id);
      return QHttpServerResponse(Status::NoContent);
    });

    server_.route("/v1/cameras/<arg>/segments", Method::Get, [this, guard](const QString& id, const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      const StubCamera* cam = camera(id);
      if (!cam) return fail(Status::NotFound, "not_found", "no such camera");
      QJsonArray arr;
      for (const fovea::RecordingSegment& s : cam->segments) arr.push_back(s.toJson());
      return json(arr);
    });

    server_.route("/v1/cameras/<arg>/gaps", Method::Get, [this, guard](const QString& id, const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      const StubCamera* cam = camera(id);
      if (!cam) return fail(Status::NotFound, "not_found", "no such camera");
      QJsonArray arr;
      for (const fovea::ReceiveGap& g : cam->gaps) arr.push_back(g.toJson());
      return json(arr);
    });

    server_.route("/v1/playback", Method::Post, [this, guard](const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      const auto b = body(req);
      if (!b || (!b->contains("segment_id") && !b->contains("at_utc_ms")))
        return fail(Status::BadRequest, "bad_request", "segment_id or camera_id + at_utc_ms required");
      const StubCamera* owner = nullptr;
      const fovea::RecordingSegment* seg = nullptr;
      int64_t atUtcMs = 0;
      if (b->contains("segment_id")) {
        seg = findSegment(b->value("segment_id").toString(), &owner);
      } else {
        owner = camera(b->value("camera_id").toString());
        atUtcMs = static_cast<int64_t>(b->value("at_utc_ms").toDouble());
        for (const fovea::RecordingSegment& candidate : owner ? owner->segments : QVector<fovea::RecordingSegment>{}) {
          if (candidate.state == QLatin1StringView("finalized") && candidate.startUtcMs <= atUtcMs && atUtcMs < candidate.endUtcMs)
            seg = &candidate;
        }
        if (!seg) return fail(Status::NotFound, "playback_failed", "no finalized recording covers that time");
      }
      if (!seg) return fail(Status::NotFound, "playback_failed", "no such segment");
      if (seg->state != QLatin1StringView("finalized")) return fail(Status::Conflict, "playback_failed", "segment is not finalized");
      auto pb = std::make_unique<StubPlayback>();
      pb->state.id = uuid();
      pb->state.segmentId = seg->id;
      pb->state.cameraId = seg->cameraId;
      pb->state.path = seg->path;
      pb->state.state = QStringLiteral("playing");
      pb->state.playing = true;
      pb->state.durationNs = (seg->endUtcMs - seg->startUtcMs) * kNsPerMs;
      pb->state.startUtcMs = seg->startUtcMs;
      if (atUtcMs > 0) {
        pb->state.positionNs = (atUtcMs - seg->startUtcMs) * kNsPerMs;
        pb->state.playing = false;
        pb->state.state = QStringLiteral("ready");
      }
      pb->ring = fovea::FrameRingWriter::create(fovea::makeRingName(QStringLiteral("stub-pb-") + pb->state.id),
                                                kRingSlots, kRingMaxWidth, kRingMaxHeight);
      if (!pb->ring) return fail(Status::InternalServerError, "playback_failed", "cannot create frame ring");
      pb->state.frameRing = ringRef(*pb->ring);
      pb->cameraCode = owner->camera.code;
      const QJsonObject created = pb->state.toJson();
      playbacks_.emplace(pb->state.id, std::move(pb));
      return json(created, Status::Created);
    });

    server_.route("/v1/playback/<arg>", Method::Get, [this, guard](const QString& id, const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      const StubPlayback* pb = playback(id);
      if (!pb) return fail(Status::NotFound, "not_found", "no such playback channel");
      return playbackJson(*pb);
    });

    server_.route("/v1/playback/<arg>", Method::Delete, [this, guard](const QString& id, const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      if (playbacks_.erase(id) == 0) return fail(Status::NotFound, "not_found", "no such playback channel");
      return QHttpServerResponse(Status::NoContent);
    });

    server_.route("/v1/playback/<arg>/<arg>", Method::Post,
                  [this, guard](const QString& id, const QString& action, const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      StubPlayback* pb = playback(id);
      if (!pb) return fail(Status::NotFound, "not_found", "no such playback channel");
      fovea::PlaybackState& s = pb->state;
      if (action == QLatin1StringView("play")) {
        if (s.positionNs >= s.durationNs) s.positionNs = 0;
        s.playing = true;
        s.state = QStringLiteral("playing");
      } else if (action == QLatin1StringView("pause")) {
        s.playing = false;
        s.state = QStringLiteral("paused");
      } else if (action == QLatin1StringView("seek")) {
        const auto b = body(req);
        if (!b || !b->contains("pts_ns")) return fail(Status::BadRequest, "bad_request", "pts_ns required");
        s.positionNs = std::clamp(static_cast<int64_t>(b->value("pts_ns").toDouble()), int64_t{0}, s.durationNs);
        if (!s.playing) s.state = QStringLiteral("paused");
      } else if (action == QLatin1StringView("rate")) {
        const auto b = body(req);
        if (!b || !b->contains("rate")) return fail(Status::BadRequest, "bad_request", "rate required");
        s.rate = b->value("rate").toDouble(1.0);
      } else {
        return fail(Status::NotFound, "not_found", "no such action");
      }
      return playbackJson(*pb);
    });

    server_.route("/v1/metrics", Method::Get, [this, guard](const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      return json(QJsonObject{{"stub", true}, {"cameras", static_cast<int>(order_.size())},
                              {"playback_channels", static_cast<int>(playbacks_.size())}});
    });

    registerM3Routes();

    server_.route("/v1/service/shutdown", Method::Post, [guard](const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      QTimer::singleShot(50, [] { QCoreApplication::quit(); });
      return json(QJsonObject{{"status", "stopping"}});
    });
  }

  QJsonArray orderedArray(const QStringList& order, const std::map<QString, QJsonObject>& items,
                          const QString& cameraFilter = {}) const {
    QJsonArray out;
    for (const QString& id : order) {
      const QJsonObject& o = items.at(id);
      if (cameraFilter.isEmpty() || o.value("camera_id").toString() == cameraFilter) out.push_back(o);
    }
    return out;
  }

  static void applyFields(QJsonObject& target, const QJsonObject& fields, std::initializer_list<const char*> protectedKeys) {
    for (auto it = fields.begin(); it != fields.end(); ++it) {
      const QByteArray key = it.key().toUtf8();
      if (std::any_of(protectedKeys.begin(), protectedKeys.end(), [&](const char* k) { return key == k; })) continue;
      target.insert(it.key(), it.value());
    }
  }

  void registerM3Routes() {
    using Method = QHttpServerRequest::Method;

    server_.route("/v1/zones", Method::Get, [this](const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      return json(orderedArray(zoneOrder_, zones_, QUrlQuery(req.url()).queryItemValue(QStringLiteral("camera_id"))));
    });

    server_.route("/v1/zones", Method::Post, [this](const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      const auto b = body(req);
      if (!b || !camera(b->value("camera_id").toString()) || b->value("points").toArray().size() < 3)
        return fail(Status::BadRequest, "invalid_zone", "camera_id and at least three points are required");
      QJsonObject zone = *b;
      zone.insert("id", uuid());
      zone.insert("revision", 1);
      zone.insert("created_utc_ms", num(fovea::utcNowMs()));
      addZone(zone);
      report(QStringLiteral("STUB zone created %1").arg(zone.value("name").toString()));
      return json(zone, Status::Created);
    });

    server_.route("/v1/zones/<arg>", Method::Put, [this](const QString& id, const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      const auto it = zones_.find(id);
      if (it == zones_.end()) return fail(Status::NotFound, "not_found", "no such zone");
      const auto b = body(req);
      if (!b) return fail(Status::BadRequest, "bad_json", "body must be a JSON object");
      applyFields(it->second, *b, {"id", "camera_id", "revision", "created_utc_ms"});
      it->second.insert("revision", it->second.value("revision").toInt() + 1);
      return json(it->second);
    });

    server_.route("/v1/rules", Method::Get, [this](const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      return json(orderedArray(ruleOrder_, rules_));
    });

    server_.route("/v1/rules", Method::Post, [this](const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      const auto b = body(req);
      if (!b || b->value("name").toString().isEmpty() || !zones_.count(b->value("zone_id").toString()))
        return fail(Status::BadRequest, "invalid_rule", "name and an existing zone_id are required");
      QJsonObject rule = *b;
      rule.insert("id", uuid());
      rule.insert("revision", 1);
      rule.insert("created_utc_ms", num(fovea::utcNowMs()));
      addRule(rule);
      report(QStringLiteral("STUB rule created %1").arg(rule.value("name").toString()));
      return json(rule, Status::Created);
    });

    server_.route("/v1/rules/<arg>", Method::Put, [this](const QString& id, const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      const auto it = rules_.find(id);
      if (it == rules_.end()) return fail(Status::NotFound, "not_found", "no such rule");
      const auto b = body(req);
      if (!b) return fail(Status::BadRequest, "bad_json", "body must be a JSON object");
      applyFields(it->second, *b, {"id", "revision", "created_utc_ms"});
      it->second.insert("revision", it->second.value("revision").toInt() + 1);
      report(QStringLiteral("STUB rule revised %1 rev %2").arg(it->second.value("name").toString()).arg(it->second.value("revision").toInt()));
      return json(it->second);
    });

    server_.route("/v1/rules/<arg>/<arg>", Method::Post,
                  [this](const QString& id, const QString& action, const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      const auto it = rules_.find(id);
      if (it == rules_.end()) return fail(Status::NotFound, "not_found", "no such rule");
      if (action != QLatin1StringView("enable") && action != QLatin1StringView("disable"))
        return fail(Status::NotFound, "not_found", "no such action");
      it->second.insert("enabled", action == QLatin1StringView("enable"));
      return json(it->second);
    });

    server_.route("/v1/test/events-delay", Method::Post, [this](const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      const auto b = body(req);
      if (!b) return fail(Status::BadRequest, "bad_json", "body must be a JSON object");
      eventsDelayMs_ = std::max(0, b->value("ms").toInt());
      return json(QJsonObject{{"ms", eventsDelayMs_}});
    });

    server_.route("/v1/events/counts", Method::Get, [this](const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      int unresolved = 0;
      int acknowledged = 0;
      int dismissed = 0;
      for (const auto& ev : events_) {
        const QString state = ev->event.value("operator_state").toString();
        const bool falseAlarm = ev->event.value("review").toObject().value("label").toString() == QLatin1StringView("false_alarm");
        if (state == QLatin1StringView("resolved") || falseAlarm) ++dismissed;
        else if (state == QLatin1StringView("acknowledged")) ++acknowledged;
        else ++unresolved;
      }
      return json(QJsonObject{{"unresolved", unresolved}, {"acknowledged", acknowledged}, {"dismissed", dismissed}});
    });

    server_.route("/v1/events", Method::Get, [this](const QHttpServerRequest& req) -> QFuture<QHttpServerResponse> {
      auto promise = std::make_shared<QPromise<QHttpServerResponse>>();
      promise->start();
      const QFuture<QHttpServerResponse> future = promise->future();
      if (!authorized(req)) {
        promise->addResult(unauthorized());
        promise->finish();
        return future;
      }
      const QUrlQuery q(req.url());
      const int64_t from = q.queryItemValue(QStringLiteral("from_utc_ms")).toLongLong();
      const int limit = q.hasQueryItem(QStringLiteral("limit")) ? q.queryItemValue(QStringLiteral("limit")).toInt() : 100;
      QVector<const StubEvent*> matching;
      for (const auto& ev : events_)
        if (static_cast<int64_t>(ev->event.value("opened_utc_ms").toDouble()) >= from) matching.push_back(ev.get());
      std::sort(matching.begin(), matching.end(), [](const StubEvent* a, const StubEvent* b) {
        return a->event.value("opened_utc_ms").toDouble() > b->event.value("opened_utc_ms").toDouble();
      });
      QJsonArray out;
      for (const StubEvent* ev : matching) {
        if (out.size() >= limit) break;
        out.push_back(ev->event);
      }
      QTimer::singleShot(eventsDelayMs_, this, [promise, out] {
        promise->addResult(json(out));
        promise->finish();
      });
      return future;
    });

    server_.route("/v1/events/<arg>", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      const StubEvent* ev = findEvent(id);
      if (!ev) return fail(Status::NotFound, "not_found", "no such event");
      QJsonObject full = ev->event;
      full.insert("evaluations", ev->evaluations);
      return json(full);
    });

    server_.route("/v1/events/<arg>/<arg>", Method::Post,
                  [this](const QString& id, const QString& action, const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      StubEvent* ev = findEvent(id);
      if (!ev) return fail(Status::NotFound, "not_found", "no such event");
      if (action == QLatin1StringView("acknowledge")) {
        if (ev->event.value("operator_state").toString() == QLatin1StringView("new")) ev->event.insert("operator_state", "acknowledged");
      } else if (action == QLatin1StringView("resolve")) {
        ev->event.insert("operator_state", "resolved");
      } else if (action == QLatin1StringView("review")) {
        const auto b = body(req);
        const QString label = b ? b->value("label").toString() : QString();
        if (label != QLatin1StringView("confirmed") && label != QLatin1StringView("false_alarm") && label != QLatin1StringView("undecided"))
          return fail(Status::BadRequest, "invalid_label", "label must be confirmed, false_alarm or undecided");
        ev->event.insert("review", QJsonObject{{"label", label}, {"note", b->value("note").toString()}, {"operator", "console"},
                                               {"utc_ms", num(fovea::utcNowMs())}});
      } else {
        return fail(Status::NotFound, "not_found", "no such action");
      }
      report(QStringLiteral("STUB event %1 %2").arg(action, ev->event.value("title").toString()));
      return json(ev->event);
    });

    server_.route("/v1/alerts/pending", Method::Get, [this](const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      maybeRaiseLiveAlert(QUrlQuery(req.url()).queryItemValue(QStringLiteral("console_id")));
      QJsonArray out;
      for (const auto& ev : events_) {
        if (ev->event.value("operator_state").toString() == QLatin1StringView("resolved")) continue;
        for (const QJsonValue& d : ev->event.value("deliveries").toArray())
          if (d.toObject().value("state").toString() != QLatin1StringView("delivered")) out.push_back(d);
      }
      return json(out);
    });

    server_.route("/v1/alerts/<arg>/delivered", Method::Post, [this](const QString& id, const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      for (const auto& ev : events_) {
        QJsonArray deliveries = ev->event.value("deliveries").toArray();
        for (qsizetype i = 0; i < deliveries.size(); ++i) {
          QJsonObject d = deliveries.at(i).toObject();
          if (d.value("id").toString() != id) continue;
          d.insert("state", "delivered");
          d.insert("attempts", d.value("attempts").toInt() + 1);
          d.insert("delivered_utc_ms", num(fovea::utcNowMs()));
          deliveries.replace(i, d);
          ev->event.insert("deliveries", deliveries);
          report(QStringLiteral("STUB delivered %1 %2").arg(d.value("channel").toString(), ev->event.value("title").toString()));
          return json(d);
        }
      }
      return fail(Status::NotFound, "not_found", "no such delivery");
    });

    server_.route("/v1/cameras/<arg>/detections/latest", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      const StubCamera* cam = camera(id);
      if (!cam) return fail(Status::NotFound, "not_found", "no such camera");
      if (!cam->camera.analyticsEnabled) return fail(Status::NotFound, "analytics_off", "analytics is off for this camera");
      if (cam->figures.size() <= static_cast<size_t>(kDetectionLagFrames))
        return fail(Status::NotFound, "no_detections", "no detection frame yet");
      const FigureSample& f = cam->figures[cam->figures.size() - 1 - static_cast<size_t>(kDetectionLagFrames)];
      const QJsonObject detection{{"track_id", "7"}, {"cls", "person"}, {"confidence", 0.94},
                                  {"bbox", QJsonArray{f.box.left(), f.box.top(), f.box.right(), f.box.bottom()}},
                                  {"anchor_foot", QJsonArray{f.box.center().x(), f.box.bottom()}},
                                  {"anchor_center", QJsonArray{f.box.center().x(), f.box.center().y()}}};
      return json(QJsonObject{{"camera_id", id}, {"session_id", cam->status.sessionId},
                              {"frame_id", QStringLiteral("stub-%1").arg(f.frame)}, {"pts_ns", num(f.ptsNs)},
                              {"utc_ms", num(f.utcMs)}, {"width", kFrameWidth}, {"height", kFrameHeight},
                              {"detections", QJsonArray{detection}}});
    });

    server_.route("/v1/evidence/<arg>/thumbnail", Method::Get, [this](const QString& id, const QHttpServerRequest& req) {
      if (!authorized(req)) return unauthorized();
      for (const auto& ev : events_) {
        if (ev->event.value("evidence").toObject().value("id").toString() != id) continue;
        if (ev->thumbnailJpeg.isEmpty()) break;
        return QHttpServerResponse(QByteArrayLiteral("image/jpeg"), ev->thumbnailJpeg);
      }
      return fail(Status::NotFound, "not_found", "no thumbnail for that evidence");
    });
  }

  static void paintFigure(QPainter& p, const QRectF& box) {
    const QRectF px(box.left() * kFrameWidth, box.top() * kFrameHeight, box.width() * kFrameWidth, box.height() * kFrameHeight);
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(236, 236, 240));
    const double head = px.width() * 0.55;
    p.drawEllipse(QRectF(px.center().x() - head / 2, px.top(), head, head));
    p.drawRoundedRect(QRectF(px.left(), px.top() + head * 1.05, px.width(), px.height() - head * 1.05), 6, 6);
    p.restore();
  }

  void tick() {
    ++frame_;
    const int64_t mono = fovea::monoNowNs();
    const int64_t utc = fovea::utcNowMs();
    for (const QString& id : order_) {
      StubCamera& cam = *camera(id);
      const QRectF figure = figureBox(cam.camera.code, frame_);
      paintCameraFrame(cam, figure);
      fovea::FrameHeader h;
      h.width = kFrameWidth;
      h.height = kFrameHeight;
      h.ptsNs = static_cast<uint64_t>(frame_) * kFrameIntervalMs * kNsPerMs;
      cam.figures.push_back({static_cast<int64_t>(h.ptsNs), utc, frame_, figure});
      if (cam.figures.size() > static_cast<size_t>(kDetectionHistory)) cam.figures.pop_front();
      h.recvMonoNs = static_cast<uint64_t>(mono);
      h.captureUtcMs = static_cast<uint64_t>(utc);
      h.sessionId = fovea::sessionIdBytes(cam.status.sessionId);
      cam.ring->write(h, canvas_.constBits(), static_cast<size_t>(canvas_.bytesPerLine()));
      cam.status.lastFrameRecvMonoNs = mono;
      cam.status.lastFrameAgeMs = 0;
    }
    for (auto& entry : playbacks_) {
      StubPlayback& pb = *entry.second;
      fovea::PlaybackState& s = pb.state;
      if (s.playing) {
        s.positionNs += static_cast<int64_t>(kFrameIntervalMs * kNsPerMs * s.rate);
        if (s.positionNs >= s.durationNs) {
          s.positionNs = s.durationNs;
          s.playing = false;
          s.state = QStringLiteral("ended");
        }
      }
      paintPlaybackFrame(pb);
      fovea::FrameHeader h;
      h.width = kFrameWidth;
      h.height = kFrameHeight;
      h.ptsNs = static_cast<uint64_t>(s.positionNs);
      h.recvMonoNs = static_cast<uint64_t>(mono);
      h.captureUtcMs = static_cast<uint64_t>(s.startUtcMs + s.positionNs / kNsPerMs);
      pb.ring->write(h, canvas_.constBits(), static_cast<size_t>(canvas_.bytesPerLine()));
    }
  }

  void ensureCanvas() {
    if (canvas_.isNull()) canvas_ = QImage(kFrameWidth, kFrameHeight, QImage::Format_ARGB32);
  }

  void paintCameraFrame(const StubCamera& cam, const QRectF& figure) {
    ensureCanvas();
    canvas_.fill(QColor(18, 22, 28));
    QPainter p(&canvas_);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(cam.tint, 6));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRect(3, 3, kFrameWidth - 7, kFrameHeight - 7));
    p.setPen(QPen(QColor(60, 70, 84), 1));
    for (int x = 64; x < kFrameWidth; x += 64) p.drawLine(x, 0, x, kFrameHeight);
    for (int y = 60; y < kFrameHeight; y += 60) p.drawLine(0, y, kFrameWidth, y);
    const int barX = static_cast<int>((frame_ * 5) % static_cast<uint64_t>(kFrameWidth));
    const int barY = static_cast<int>((frame_ * 3) % static_cast<uint64_t>(kFrameHeight));
    p.fillRect(QRect(barX, 0, 36, kFrameHeight), cam.tint);
    p.fillRect(QRect(0, barY, kFrameWidth, 18), QColor(255, 255, 255, 180));
    paintFigure(p, figure);
    QFont big;
    big.setPixelSize(30);
    big.setBold(true);
    p.setFont(big);
    p.setPen(Qt::white);
    p.drawText(QRect(24, 24, kFrameWidth - 48, 40), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("%1  frame %2").arg(cam.camera.code).arg(frame_, 6, 10, QLatin1Char('0')));
    QFont small;
    small.setPixelSize(18);
    p.setFont(small);
    p.drawText(QRect(24, kFrameHeight - 60, kFrameWidth - 48, 30), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("STUB CORE · TEST PATTERN · %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))));
  }

  void paintPlaybackFrame(const StubPlayback& pb) {
    ensureCanvas();
    canvas_.fill(QColor(20, 18, 32));
    QPainter p(&canvas_);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor tint(120, 160, 255);
    p.setPen(QPen(tint, 6));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRect(3, 3, kFrameWidth - 7, kFrameHeight - 7));
    const double fraction = pb.state.durationNs > 0 ? static_cast<double>(pb.state.positionNs) / static_cast<double>(pb.state.durationNs) : 0.0;
    const int cx = 40 + static_cast<int>((kFrameWidth - 80) * fraction);
    p.setPen(Qt::NoPen);
    p.setBrush(tint);
    p.drawEllipse(QPoint(cx, kFrameHeight / 2), 34, 34);
    p.fillRect(QRect(40, kFrameHeight - 50, kFrameWidth - 80, 10), QColor(60, 60, 90));
    p.fillRect(QRect(40, kFrameHeight - 50, static_cast<int>((kFrameWidth - 80) * fraction), 10), Qt::white);
    QFont big;
    big.setPixelSize(30);
    big.setBold(true);
    p.setFont(big);
    p.setPen(Qt::white);
    const int64_t posS = pb.state.positionNs / 1'000'000'000;
    const int64_t durS = pb.state.durationNs / 1'000'000'000;
    p.drawText(QRect(24, 24, kFrameWidth - 48, 40), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("PLAYBACK %1  %2 / %3 s").arg(pb.cameraCode).arg(posS).arg(durS));
    QFont small;
    small.setPixelSize(18);
    p.setFont(small);
    p.drawText(QRect(24, 70, kFrameWidth - 48, 30), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("STUB CORE · %1 · %2").arg(pb.state.segmentId, pb.state.state));
  }

  QString dataDir_;
  QString token_;
  QHttpServer server_;
  QTcpServer* tcp_ = nullptr;
  quint16 port_ = 0;
  int64_t startedUtcMs_ = fovea::utcNowMs();
  std::map<QString, std::unique_ptr<StubCamera>> cameras_;
  QStringList order_;
  std::map<QString, std::unique_ptr<StubPlayback>> playbacks_;
  std::map<QString, QJsonObject> zones_;
  QStringList zoneOrder_;
  std::map<QString, QJsonObject> rules_;
  QStringList ruleOrder_;
  std::vector<std::unique_ptr<StubEvent>> events_;
  // Console id -> first pending poll (utc ms); 0 once its live alert was raised.
  std::map<QString, int64_t> consolesSeen_;
  // Set by POST /v1/test/events-delay: how long GET /v1/events holds its answer.
  int eventsDelayMs_ = 0;
  QTimer frameTimer_;
  QImage canvas_;
  uint64_t frame_ = 0;
};

#ifndef _WIN32
int g_signalFds[2] = {-1, -1};
void onSignal(int) {
  const char c = 1;
  const ssize_t r = ::write(g_signalFds[0], &c, 1);
  (void)r;
}
#endif

}

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("console-stub-core"));

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("STUB CORE - test fixture, not real data"));
  parser.addHelpOption();
  parser.addOption({QStringLiteral("data-dir"), QStringLiteral("Directory for core.json and core.token"), QStringLiteral("path")});
  parser.process(app);
  QString dataDir = parser.value(QStringLiteral("data-dir"));
  if (dataDir.isEmpty()) dataDir = QString::fromLocal8Bit(qgetenv("FOVEA_DATA_DIR"));
  if (dataDir.isEmpty()) {
    QTextStream(stderr) << "console-stub-core: --data-dir is required\n";
    return 2;
  }

  QTextStream out(stdout);
  out << "STUB CORE - test fixture, not real data\n";
  out.flush();

  StubCore core(dataDir);
  if (!core.start()) {
    QTextStream(stderr) << "console-stub-core: cannot start in " << dataDir << "\n";
    return 3;
  }
  out << "listening on 127.0.0.1:" << core.port() << " data-dir " << dataDir << "\n";
  out.flush();

#ifndef _WIN32
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, g_signalFds) == 0) {
    auto* notifier = new QSocketNotifier(g_signalFds[1], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [] { QCoreApplication::quit(); });
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
  }
#endif
  return app.exec();
}

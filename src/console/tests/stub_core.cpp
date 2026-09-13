// STUB CORE - test fixture, not real data.
//
// Fakes enough of docs/API.md for console UI verification: two cameras with a
// moving test pattern in their frame rings, canned segments and gaps, and
// playback channels that render a pattern with a running clock. Nothing here
// touches a real stream, file or database. POST /v1/cameras/test fails for any
// main_url containing "fail" (the second seeded camera) and succeeds otherwise.
#include "fovea/Api.h"
#include "fovea/Clock.h"
#include "fovea/FrameRing.h"
#include "fovea/Token.h"
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QTcpServer>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <csignal>
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

struct StubCamera {
  fovea::Camera camera;
  fovea::CameraStatus status;
  std::unique_ptr<fovea::FrameRingWriter> ring;
  QColor tint;
  QVector<fovea::RecordingSegment> segments;
  QVector<fovea::ReceiveGap> gaps;
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
      if (!b || !b->contains("segment_id")) return fail(Status::BadRequest, "bad_request", "segment_id required");
      const StubCamera* owner = nullptr;
      const fovea::RecordingSegment* seg = findSegment(b->value("segment_id").toString(), &owner);
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

    server_.route("/v1/service/shutdown", Method::Post, [guard](const QHttpServerRequest& req) {
      std::optional<QHttpServerResponse> denied;
      guard(req, denied);
      if (denied) return std::move(*denied);
      QTimer::singleShot(50, [] { QCoreApplication::quit(); });
      return json(QJsonObject{{"status", "stopping"}});
    });
  }

  void tick() {
    ++frame_;
    const int64_t mono = fovea::monoNowNs();
    const int64_t utc = fovea::utcNowMs();
    for (const QString& id : order_) {
      StubCamera& cam = *camera(id);
      paintCameraFrame(cam);
      fovea::FrameHeader h;
      h.width = kFrameWidth;
      h.height = kFrameHeight;
      h.ptsNs = static_cast<uint64_t>(frame_) * kFrameIntervalMs * kNsPerMs;
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

  void paintCameraFrame(const StubCamera& cam) {
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

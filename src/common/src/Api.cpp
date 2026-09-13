#include "fovea/Api.h"
#include <QJsonValue>

namespace fovea {
namespace {
int64_t i64(const QJsonObject& o, const char* k, int64_t d = 0) {
  const QJsonValue v = o.value(QLatin1String(k));
  return v.isDouble() ? static_cast<int64_t>(v.toDouble()) : d;
}
int i32(const QJsonObject& o, const char* k, int d = 0) { return o.value(QLatin1String(k)).toInt(d); }
double dbl(const QJsonObject& o, const char* k, double d = 0) { return o.value(QLatin1String(k)).toDouble(d); }
bool bl(const QJsonObject& o, const char* k, bool d = false) { return o.value(QLatin1String(k)).toBool(d); }
QString str(const QJsonObject& o, const char* k, const QString& d = {}) { return o.value(QLatin1String(k)).toString(d); }
}

QJsonObject Camera::toJson() const {
  return {{"id", id}, {"code", code}, {"name", name}, {"group_name", groupName}, {"kind", kind},
          {"main_url", mainUrl}, {"sub_url", subUrl}, {"transport", transport},
          {"timeout_ms", timeoutMs}, {"jitter_ms", jitterMs}, {"segment_seconds", segmentSeconds},
          {"analytics_enabled", analyticsEnabled}, {"record_enabled", recordEnabled},
          {"enabled", enabled}, {"retention_days", retentionDays}, {"max_bytes", static_cast<double>(maxBytes)},
          {"created_utc_ms", static_cast<double>(createdUtcMs)},
          {"updated_utc_ms", static_cast<double>(updatedUtcMs)}};
}
Camera Camera::fromJson(const QJsonObject& o) {
  Camera c;
  c.id = str(o, "id");
  c.code = str(o, "code");
  c.name = str(o, "name");
  c.groupName = str(o, "group_name");
  c.kind = str(o, "kind", QStringLiteral("rtsp"));
  c.mainUrl = str(o, "main_url");
  c.subUrl = str(o, "sub_url");
  c.transport = str(o, "transport", QStringLiteral("tcp"));
  c.timeoutMs = i32(o, "timeout_ms", 8000);
  c.jitterMs = i32(o, "jitter_ms", 1000);
  c.segmentSeconds = i32(o, "segment_seconds", 60);
  c.analyticsEnabled = bl(o, "analytics_enabled", false);
  c.recordEnabled = bl(o, "record_enabled", true);
  c.enabled = bl(o, "enabled", true);
  c.retentionDays = i32(o, "retention_days", 7);
  c.maxBytes = i64(o, "max_bytes", 0);
  c.createdUtcMs = i64(o, "created_utc_ms");
  c.updatedUtcMs = i64(o, "updated_utc_ms");
  return c;
}

QJsonObject LatencyStats::toJson() const { return {{"p50", p50Ms}, {"p95", p95Ms}, {"p99", p99Ms}}; }
LatencyStats LatencyStats::fromJson(const QJsonObject& o) {
  return {dbl(o, "p50"), dbl(o, "p95"), dbl(o, "p99")};
}

QJsonObject RingRef::toJson() const {
  return {{"name", name}, {"slots", static_cast<int>(slotCount)}, {"slot_bytes", static_cast<double>(slotBytes)},
          {"max_width", static_cast<int>(maxWidth)}, {"max_height", static_cast<int>(maxHeight)}, {"format", format}};
}
RingRef RingRef::fromJson(const QJsonObject& o) {
  RingRef r;
  r.name = str(o, "name");
  r.slotCount = static_cast<uint32_t>(i32(o, "slots"));
  r.slotBytes = static_cast<uint32_t>(i64(o, "slot_bytes"));
  r.maxWidth = static_cast<uint32_t>(i32(o, "max_width"));
  r.maxHeight = static_cast<uint32_t>(i32(o, "max_height"));
  r.format = str(o, "format", QStringLiteral("BGRA"));
  return r;
}

QJsonObject CameraStatus::toJson() const {
  QJsonObject o{{"camera_id", cameraId}, {"state", state}, {"session_id", sessionId},
                {"since_utc_ms", static_cast<double>(sinceUtcMs)},
                {"last_frame_recv_mono_ns", static_cast<double>(lastFrameRecvMonoNs)},
                {"last_frame_age_ms", static_cast<double>(lastFrameAgeMs)}, {"stale", stale},
                {"codec", codec}, {"width", width}, {"height", height}, {"fps_new", fpsNew},
                {"latency_ms", latency.toJson()}, {"drops", static_cast<double>(drops)},
                {"queue_depth", queueDepth}, {"reconnects", reconnects}, {"recording", recording},
                {"current_segment_id", currentSegmentId}, {"last_error", lastError}};
  if (frameRing) o.insert("frame_ring", frameRing->toJson());
  return o;
}
CameraStatus CameraStatus::fromJson(const QJsonObject& o) {
  CameraStatus s;
  s.cameraId = str(o, "camera_id");
  s.state = str(o, "state", QStringLiteral("disabled"));
  s.sessionId = str(o, "session_id");
  s.sinceUtcMs = i64(o, "since_utc_ms");
  s.lastFrameRecvMonoNs = i64(o, "last_frame_recv_mono_ns");
  s.lastFrameAgeMs = i64(o, "last_frame_age_ms", -1);
  s.stale = bl(o, "stale", true);
  s.codec = str(o, "codec");
  s.width = i32(o, "width");
  s.height = i32(o, "height");
  s.fpsNew = dbl(o, "fps_new");
  s.latency = LatencyStats::fromJson(o.value("latency_ms").toObject());
  s.drops = i64(o, "drops");
  s.queueDepth = i32(o, "queue_depth");
  s.reconnects = i32(o, "reconnects");
  s.recording = str(o, "recording", QStringLiteral("disabled"));
  s.currentSegmentId = str(o, "current_segment_id");
  s.lastError = str(o, "last_error");
  if (o.contains("frame_ring")) s.frameRing = RingRef::fromJson(o.value("frame_ring").toObject());
  return s;
}

QJsonObject StreamSession::toJson() const {
  return {{"id", id}, {"camera_id", cameraId}, {"started_mono_ns", static_cast<double>(startedMonoNs)},
          {"started_utc_ms", static_cast<double>(startedUtcMs)}, {"first_pts_ns", static_cast<double>(firstPtsNs)},
          {"ended_utc_ms", static_cast<double>(endedUtcMs)}, {"end_reason", endReason}, {"codec", codec},
          {"width", width}, {"height", height}, {"fps", fps}, {"transport", transport},
          {"capture_clock", captureClock}};
}
StreamSession StreamSession::fromJson(const QJsonObject& o) {
  StreamSession s;
  s.id = str(o, "id");
  s.cameraId = str(o, "camera_id");
  s.startedMonoNs = i64(o, "started_mono_ns");
  s.startedUtcMs = i64(o, "started_utc_ms");
  s.firstPtsNs = i64(o, "first_pts_ns", -1);
  s.endedUtcMs = i64(o, "ended_utc_ms");
  s.endReason = str(o, "end_reason");
  s.codec = str(o, "codec");
  s.width = i32(o, "width");
  s.height = i32(o, "height");
  s.fps = dbl(o, "fps");
  s.transport = str(o, "transport");
  s.captureClock = str(o, "capture_clock", QStringLiteral("none"));
  return s;
}

QJsonObject RecordingSegment::toJson() const {
  return {{"id", id}, {"camera_id", cameraId}, {"session_id", sessionId}, {"path", path}, {"state", state},
          {"start_pts_ns", static_cast<double>(startPtsNs)}, {"end_pts_ns", static_cast<double>(endPtsNs)},
          {"start_utc_ms", static_cast<double>(startUtcMs)}, {"end_utc_ms", static_cast<double>(endUtcMs)},
          {"bytes", static_cast<double>(bytes)}, {"created_utc_ms", static_cast<double>(createdUtcMs)},
          {"finalized_utc_ms", static_cast<double>(finalizedUtcMs)}};
}
RecordingSegment RecordingSegment::fromJson(const QJsonObject& o) {
  RecordingSegment s;
  s.id = str(o, "id");
  s.cameraId = str(o, "camera_id");
  s.sessionId = str(o, "session_id");
  s.path = str(o, "path");
  s.state = str(o, "state", QStringLiteral("recording"));
  s.startPtsNs = i64(o, "start_pts_ns");
  s.endPtsNs = i64(o, "end_pts_ns");
  s.startUtcMs = i64(o, "start_utc_ms");
  s.endUtcMs = i64(o, "end_utc_ms");
  s.bytes = i64(o, "bytes");
  s.createdUtcMs = i64(o, "created_utc_ms");
  s.finalizedUtcMs = i64(o, "finalized_utc_ms");
  return s;
}

QJsonObject ReceiveGap::toJson() const {
  return {{"id", id}, {"camera_id", cameraId}, {"session_id", sessionId},
          {"from_utc_ms", static_cast<double>(fromUtcMs)}, {"to_utc_ms", static_cast<double>(toUtcMs)},
          {"reason", reason}};
}
ReceiveGap ReceiveGap::fromJson(const QJsonObject& o) {
  ReceiveGap g;
  g.id = str(o, "id");
  g.cameraId = str(o, "camera_id");
  g.sessionId = str(o, "session_id");
  g.fromUtcMs = i64(o, "from_utc_ms");
  g.toUtcMs = i64(o, "to_utc_ms");
  g.reason = str(o, "reason");
  return g;
}

QJsonObject PlaybackState::toJson() const {
  QJsonObject o{{"id", id}, {"segment_id", segmentId}, {"camera_id", cameraId}, {"path", path}, {"state", state},
                {"playing", playing}, {"rate", rate}, {"position_ns", static_cast<double>(positionNs)},
                {"duration_ns", static_cast<double>(durationNs)}, {"start_utc_ms", static_cast<double>(startUtcMs)},
                {"last_error", lastError}};
  if (frameRing) o.insert("frame_ring", frameRing->toJson());
  return o;
}
PlaybackState PlaybackState::fromJson(const QJsonObject& o) {
  PlaybackState p;
  p.id = str(o, "id");
  p.segmentId = str(o, "segment_id");
  p.cameraId = str(o, "camera_id");
  p.path = str(o, "path");
  p.state = str(o, "state", QStringLiteral("opening"));
  p.playing = bl(o, "playing");
  p.rate = dbl(o, "rate", 1.0);
  p.positionNs = i64(o, "position_ns");
  p.durationNs = i64(o, "duration_ns");
  p.startUtcMs = i64(o, "start_utc_ms");
  p.lastError = str(o, "last_error");
  if (o.contains("frame_ring")) p.frameRing = RingRef::fromJson(o.value("frame_ring").toObject());
  return p;
}

QJsonObject ConnectionTest::toJson() const {
  QJsonObject o{{"ok", ok}, {"error", error}, {"handshake_ms", handshakeMs}, {"codec", codec},
                {"width", width}, {"height", height}, {"fps", fps}, {"bitrate_kbps", bitrateKbps}};
  if (!previewJpegBase64.isEmpty()) o.insert("preview_jpeg_base64", previewJpegBase64);
  return o;
}
ConnectionTest ConnectionTest::fromJson(const QJsonObject& o) {
  ConnectionTest t;
  t.ok = bl(o, "ok");
  t.error = str(o, "error");
  t.handshakeMs = i32(o, "handshake_ms");
  t.codec = str(o, "codec");
  t.width = i32(o, "width");
  t.height = i32(o, "height");
  t.fps = dbl(o, "fps");
  t.bitrateKbps = dbl(o, "bitrate_kbps");
  t.previewJpegBase64 = str(o, "preview_jpeg_base64");
  return t;
}

QJsonObject errorJson(const QString& code, const QString& message) {
  return {{"error", QJsonObject{{"code", code}, {"message", message}}}};
}

}

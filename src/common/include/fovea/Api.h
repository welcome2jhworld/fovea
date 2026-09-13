#pragma once
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <optional>

namespace fovea {

struct Camera {
  QString id;
  QString code;
  QString name;
  QString groupName;
  QString kind = QStringLiteral("rtsp");
  QString mainUrl;
  QString subUrl;
  QString transport = QStringLiteral("tcp");
  int timeoutMs = 8000;
  int jitterMs = 1000;
  int segmentSeconds = 60;
  bool analyticsEnabled = false;
  bool recordEnabled = true;
  bool enabled = true;
  int retentionDays = 7;
  int64_t maxBytes = 0;
  int64_t createdUtcMs = 0;
  int64_t updatedUtcMs = 0;

  QJsonObject toJson() const;
  static Camera fromJson(const QJsonObject& o);
};

struct LatencyStats {
  double p50Ms = 0;
  double p95Ms = 0;
  double p99Ms = 0;
  QJsonObject toJson() const;
  static LatencyStats fromJson(const QJsonObject& o);
};

struct RingRef {
  QString name;
  uint32_t slotCount = 0;
  uint32_t slotBytes = 0;
  uint32_t maxWidth = 0;
  uint32_t maxHeight = 0;
  QString format = QStringLiteral("BGRA");
  QJsonObject toJson() const;
  static RingRef fromJson(const QJsonObject& o);
};

struct CameraStatus {
  QString cameraId;
  QString state = QStringLiteral("disabled");
  QString sessionId;
  int64_t sinceUtcMs = 0;
  int64_t lastFrameRecvMonoNs = 0;
  int64_t lastFrameAgeMs = -1;
  bool stale = true;
  QString codec;
  int width = 0;
  int height = 0;
  double fpsNew = 0;
  LatencyStats latency;
  int64_t drops = 0;
  int queueDepth = 0;
  int reconnects = 0;
  QString recording = QStringLiteral("disabled");
  QString currentSegmentId;
  QString lastError;
  std::optional<RingRef> frameRing;

  QJsonObject toJson() const;
  static CameraStatus fromJson(const QJsonObject& o);
};

struct StreamSession {
  QString id;
  QString cameraId;
  int64_t startedMonoNs = 0;
  int64_t startedUtcMs = 0;
  int64_t firstPtsNs = -1;
  int64_t endedUtcMs = 0;
  QString endReason;
  QString codec;
  int width = 0;
  int height = 0;
  double fps = 0;
  QString transport;
  QString captureClock = QStringLiteral("none");

  QJsonObject toJson() const;
  static StreamSession fromJson(const QJsonObject& o);
};

struct RecordingSegment {
  QString id;
  QString cameraId;
  QString sessionId;
  QString path;
  QString state = QStringLiteral("recording");
  int64_t startPtsNs = 0;
  int64_t endPtsNs = 0;
  int64_t startUtcMs = 0;
  int64_t endUtcMs = 0;
  int64_t bytes = 0;
  int64_t createdUtcMs = 0;
  int64_t finalizedUtcMs = 0;

  QJsonObject toJson() const;
  static RecordingSegment fromJson(const QJsonObject& o);
};

struct ReceiveGap {
  QString id;
  QString cameraId;
  QString sessionId;
  int64_t fromUtcMs = 0;
  int64_t toUtcMs = 0;
  QString reason;

  QJsonObject toJson() const;
  static ReceiveGap fromJson(const QJsonObject& o);
};

struct PlaybackState {
  QString id;
  QString segmentId;
  QString cameraId;
  QString path;
  QString state = QStringLiteral("opening");
  bool playing = false;
  double rate = 1.0;
  int64_t positionNs = 0;
  int64_t durationNs = 0;
  int64_t startUtcMs = 0;
  std::optional<RingRef> frameRing;
  QString lastError;

  QJsonObject toJson() const;
  static PlaybackState fromJson(const QJsonObject& o);
};

struct ConnectionTest {
  bool ok = false;
  QString error;
  int handshakeMs = 0;
  QString codec;
  int width = 0;
  int height = 0;
  double fps = 0;
  double bitrateKbps = 0;
  QString previewJpegBase64;

  QJsonObject toJson() const;
  static ConnectionTest fromJson(const QJsonObject& o);
};

QJsonObject errorJson(const QString& code, const QString& message);

}

#include "fovea/core/ImportManager.h"
#include "fovea/Clock.h"
#include "fovea/Ids.h"
#include "fovea/core/MediaProbe.h"
#include "fovea/core/Store.h"
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QMetaObject>
#include <QThreadPool>
#include <gst/gst.h>
#include <limits>
#include <mutex>
#include <utility>

namespace fovea::core {
namespace {

constexpr int kProgressIntervalMs = 500;
const QStringList kContainers{QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"), QStringLiteral("mkv")};

ServiceError error(int status, const QString& code, const QString& message) { return ServiceError{status, code, message}; }

bool readTime(const GstStructure* s, const char* field, int64_t& out) {
  guint64 value = 0;
  if (!gst_structure_get_uint64(s, field, &value) || value > static_cast<guint64>(std::numeric_limits<int64_t>::max())) return false;
  out = static_cast<int64_t>(value);
  return true;
}

QString detailJson(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

// Links the demuxer's first video pad to the parser; other tracks stay unlinked.
void linkVideoPad(GstElement*, GstPad* pad, gpointer parser) {
  GstCaps* caps = gst_pad_query_caps(pad, nullptr);
  const bool video = caps && gst_caps_get_size(caps) > 0 && g_str_has_prefix(gst_structure_get_name(gst_caps_get_structure(caps, 0)), "video/");
  if (caps) gst_caps_unref(caps);
  GstPad* sink = gst_element_get_static_pad(static_cast<GstElement*>(parser), "sink");
  if (video && !gst_pad_is_linked(sink)) gst_pad_link(pad, sink);
  gst_object_unref(sink);
}

}

struct ImportManager::Mailbox {
  std::mutex mutex;
  ImportManager* target = nullptr;
  std::deque<GstMessage*> messages;

  void post(std::function<void(ImportManager&)> fn) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!target) return;
    ImportManager* t = target;
    QMetaObject::invokeMethod(t, [t, fn = std::move(fn)] { fn(*t); }, Qt::QueuedConnection);
  }
  void detach() {
    std::lock_guard<std::mutex> lock(mutex);
    target = nullptr;
    for (GstMessage* m : messages) gst_message_unref(m);
    messages.clear();
  }
};

struct ImportManager::Active {
  ImportRecord record;
  QString recordDir;
  GstElement* pipeline = nullptr;
  QHash<uint, QString> openSegments;

  ~Active() {
    if (pipeline) {
      GstBus* bus = gst_element_get_bus(pipeline);
      gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
      gst_object_unref(bus);
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
    }
  }

  int64_t utcFor(int64_t runningTimeNs) const { return record.startUtcMs + runningTimeNs / 1'000'000; }

  static gchar* onFormatLocation(GstElement*, guint fragmentId, gpointer user) {
    auto* active = static_cast<Active*>(user);
    const QString path = QStringLiteral("%1/%2_%3.mkv").arg(active->recordDir, active->record.sessionId,
                                                            QString::number(fragmentId).rightJustified(5, QLatin1Char('0')));
    return g_strdup(path.toUtf8().constData());
  }
};

ImportManager::ImportManager(Store& store, CoreConfig config, QObject* parent)
    : QObject(parent), store_(store), config_(std::move(config)), mailbox_(std::make_shared<Mailbox>()) {
  mailbox_->target = this;
  progressTimer_.setInterval(kProgressIntervalMs);
  connect(&progressTimer_, &QTimer::timeout, this, &ImportManager::updateProgress);
}

ImportManager::~ImportManager() {
  mailbox_->detach();
  active_.reset();
}

int ImportManager::removeSessionFiles(const QString& cameraId, const QString& sessionId) {
  if (cameraId.isEmpty() || sessionId.isEmpty()) return 0;
  QDir dir(config_.recordingsDir + QLatin1Char('/') + cameraId);
  int removed = 0;
  for (const QString& name : dir.entryList({sessionId + QStringLiteral("_*.mkv")}, QDir::Files))
    if (dir.remove(name)) ++removed;
  return removed;
}

void ImportManager::recover() {
  for (ImportRecord r : store_.listImports(std::numeric_limits<int>::max())) {
    if (r.state != QLatin1String("queued") && r.state != QLatin1String("running")) continue;
    MarkedEvidence marked;
    if (!r.sessionId.isEmpty() && !store_.deleteSessionSegments(r.sessionId, QStringLiteral("import_failed"), utcNowMs(), &marked))
      qWarning("import %s: cannot delete its segments: %s", qPrintable(r.id), qPrintable(store_.lastError()));
    Store::removeEvidenceFiles(marked.thumbnails, config_.dataDir + QStringLiteral("/evidence"));
    Store::removeIndexFiles(marked.indexThumbnails, config_.dataDir + QStringLiteral("/index"));
    if (!r.sessionId.isEmpty()) store_.endSession(r.sessionId, QStringLiteral("error"), utcNowMs());
    removeSessionFiles(r.cameraId, r.sessionId);
    r.state = QStringLiteral("failed");
    r.error = QStringLiteral("core_restart");
    r.finishedUtcMs = utcNowMs();
    store_.updateImport(r);
    qWarning("import %s: left unfinished by the previous run, marked failed", qPrintable(r.id));
  }
  store_.purgeDeletedSegmentFiles(config_.recordingsDir, utcNowMs(), std::numeric_limits<int>::max());
}

void ImportManager::submit(const QString& path, const QString& cameraId, int64_t startUtcMs, Done done) {
  const auto reject = [&done](const ServiceError& e) { done(std::nullopt, e); };
  const QFileInfo file(path);
  if (path.isEmpty() || !file.isAbsolute()) return reject(error(400, "invalid_import", "path must be an absolute file path"));
  if (startUtcMs <= 0) return reject(error(400, "invalid_import", "start_utc_ms must be a positive UTC time"));
  const std::optional<Camera> camera = store_.getCamera(cameraId);
  if (!camera) return reject(error(404, "not_found", "no such camera"));
  if (!file.exists() || !file.isFile()) return reject(error(400, "file_not_found", "no file at that path"));
  if (!file.isReadable()) return reject(error(400, "file_unreadable", "the file cannot be read"));
  if (!kContainers.contains(file.suffix().toLower()))
    return reject(error(400, "unsupported_container", "only MP4, MOV and MKV files can be imported"));

  ImportRecord pending;
  pending.id = newId();
  pending.cameraId = cameraId;
  pending.sourcePath = file.absoluteFilePath();
  pending.startUtcMs = startUtcMs;
  pending.createdUtcMs = utcNowMs();
  const std::shared_ptr<Mailbox> mailbox = mailbox_;
  QThreadPool::globalInstance()->start([mailbox, pending, done = std::move(done)]() mutable {
    const MediaInfo media = probeMedia(pending.sourcePath);
    pending.codec = media.codec;
    pending.durationNs = media.durationNs;
    mailbox->post([pending, media, done = std::move(done)](ImportManager& self) mutable {
      self.onProbed(pending, media.width, media.height, std::move(done));
    });
  });
}

void ImportManager::onProbed(const ImportRecord& probed, int width, int height, Done done) {
  ImportRecord r = probed;
  if (r.codec.isEmpty()) return done(std::nullopt, error(400, "unreadable_media", "no readable video track in the file"));
  if (r.codec != QLatin1String("h264") && r.codec != QLatin1String("h265"))
    return done(std::nullopt, error(400, "unsupported_codec",
                                    QStringLiteral("video codec %1 is not H.264 or H.265").arg(r.codec.mid(r.codec.indexOf(':') + 1))));
  if (r.durationNs <= 0) return done(std::nullopt, error(400, "unreadable_media", "the file has no duration"));
  const std::optional<Camera> camera = store_.getCamera(r.cameraId);
  if (!camera) return done(std::nullopt, error(404, "not_found", "no such camera"));
  const int64_t endUtcMs = r.startUtcMs + r.durationNs / 1'000'000 + 1;
  bool overlaps = store_.cameraHasFootage(r.cameraId, r.startUtcMs, endUtcMs);
  for (const QString& id : queue_) {
    const auto other = store_.getImport(id);
    if (other && other->cameraId == r.cameraId && other->startUtcMs < endUtcMs &&
        other->startUtcMs + other->durationNs / 1'000'000 + 1 > r.startUtcMs)
      overlaps = true;
  }
  if (active_ && active_->record.cameraId == r.cameraId && active_->record.startUtcMs < endUtcMs &&
      active_->record.startUtcMs + active_->record.durationNs / 1'000'000 + 1 > r.startUtcMs)
    overlaps = true;
  if (overlaps)
    return done(std::nullopt, error(409, "footage_overlap", "the camera already has footage in that time range"));
  r.createdUtcMs = utcNowMs();
  if (!store_.insertImport(r)) return done(std::nullopt, error(500, "store_failed", store_.lastError()));
  sizes_.insert(r.id, {width, height});
  store_.appendAudit(QStringLiteral("api"), QStringLiteral("import.create"), r.id,
                     detailJson({{"camera_id", r.cameraId},
                                 {"file", QFileInfo(r.sourcePath).fileName()},
                                 {"start_utc_ms", static_cast<double>(r.startUtcMs)},
                                 {"duration_ns", static_cast<double>(r.durationNs)},
                                 {"codec", r.codec}}),
                     r.createdUtcMs);
  qInfo("import %s: queued %s for camera %s (%s, %.1f s)", qPrintable(r.id), qPrintable(QFileInfo(r.sourcePath).fileName()),
        qPrintable(r.cameraId), qPrintable(r.codec), static_cast<double>(r.durationNs) / 1e9);
  queue_.push_back(r.id);
  done(r, ServiceError{});
  startNext();
}

void ImportManager::startNext() {
  while (!active_ && !queue_.empty()) {
    const QString id = queue_.front();
    queue_.pop_front();
    std::optional<ImportRecord> record = store_.getImport(id);
    if (!record || record->state != QLatin1String("queued")) continue;
    const std::optional<Camera> camera = store_.getCamera(record->cameraId);
    const auto size = sizes_.take(id);
    auto active = std::make_unique<Active>();
    active->record = *record;
    active->record.sessionId = newId();
    active->record.state = QStringLiteral("running");
    active->record.startedUtcMs = utcNowMs();
    active->recordDir = config_.recordingsDir + QLatin1Char('/') + record->cameraId;
    if (!camera) {
      active_ = std::move(active);
      finish(QStringLiteral("camera deleted"));
      continue;
    }

    StreamSession session;
    session.id = active->record.sessionId;
    session.cameraId = record->cameraId;
    session.startedUtcMs = record->startUtcMs;
    session.firstPtsNs = 0;
    session.transport = QStringLiteral("file");
    session.captureClock = QStringLiteral("imported");
    if (!store_.insertSession(session) ||
        !store_.setSessionMedia(session.id, codecDisplayName(record->codec), size.first, size.second, 0, QStringLiteral("imported"))) {
      active_ = std::move(active);
      finish(QStringLiteral("cannot store the session: ") + store_.lastError());
      continue;
    }
    store_.updateImport(active->record);

    const bool h264 = record->codec == QLatin1String("h264");
    const bool mkv = QFileInfo(record->sourcePath).suffix().compare(QLatin1String("mkv"), Qt::CaseInsensitive) == 0;
    GstElement* source = gst_element_factory_make("filesrc", nullptr);
    GstElement* demux = gst_element_factory_make(mkv ? "matroskademux" : "qtdemux", nullptr);
    GstElement* parse = gst_element_factory_make(h264 ? "h264parse" : "h265parse", nullptr);
    GstElement* split = gst_element_factory_make("splitmuxsink", nullptr);
    GstElement* muxer = gst_element_factory_make("matroskamux", nullptr);
    if (!source || !demux || !parse || !split || !muxer || !QDir().mkpath(active->recordDir)) {
      for (GstElement* e : {source, demux, parse, split, muxer})
        if (e) gst_object_unref(e);
      active_ = std::move(active);
      finish(QStringLiteral("cannot build the import pipeline"));
      continue;
    }
    active->pipeline = gst_pipeline_new(nullptr);
    g_object_set(source, "location", record->sourcePath.toUtf8().constData(), nullptr);
    // The muxer must be set before the video pad is requested, or splitmuxsink creates its default mp4mux.
    g_object_set(split, "max-size-time", static_cast<guint64>(camera->segmentSeconds) * GST_SECOND, "muxer", muxer, nullptr);
    g_signal_connect(split, "format-location", G_CALLBACK(Active::onFormatLocation), active.get());
    gst_bin_add_many(GST_BIN(active->pipeline), source, demux, parse, split, nullptr);
    gst_element_link(source, demux);
    gst_element_link_pads(parse, "src", split, "video");
    g_signal_connect(demux, "pad-added", G_CALLBACK(linkVideoPad), parse);

    GstBus* bus = gst_element_get_bus(active->pipeline);
    gst_bus_set_sync_handler(
        bus,
        [](GstBus*, GstMessage* msg, gpointer user) -> GstBusSyncReply {
          const GstMessageType type = GST_MESSAGE_TYPE(msg);
          if (type != GST_MESSAGE_ERROR && type != GST_MESSAGE_EOS && type != GST_MESSAGE_ELEMENT) return GST_BUS_DROP;
          auto* mailbox = static_cast<Mailbox*>(user);
          {
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            if (!mailbox->target) return GST_BUS_DROP;
            mailbox->messages.push_back(gst_message_ref(msg));
          }
          mailbox->post([](ImportManager& self) { self.drain(); });
          return GST_BUS_DROP;
        },
        mailbox_.get(), nullptr);
    gst_object_unref(bus);

    active_ = std::move(active);
    qInfo("import %s: started, session %s", qPrintable(active_->record.id), qPrintable(active_->record.sessionId));
    if (gst_element_set_state(active_->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      finish(QStringLiteral("import pipeline refused to start"));
      continue;
    }
    progressTimer_.start();
  }
}

void ImportManager::drain() {
  std::deque<GstMessage*> pending;
  {
    std::lock_guard<std::mutex> lock(mailbox_->mutex);
    pending.swap(mailbox_->messages);
  }
  for (GstMessage* msg : pending) {
    if (active_) handle(msg);
    gst_message_unref(msg);
  }
}

void ImportManager::handle(GstMessage* msg) {
  Active& a = *active_;
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gst_message_parse_error(msg, &err, nullptr);
      const QString text = QString::fromUtf8(err ? err->message : "import pipeline error");
      if (err) g_error_free(err);
      finish(text);
      return;
    }
    case GST_MESSAGE_EOS:
      finish(QString());
      return;
    case GST_MESSAGE_ELEMENT:
      break;
    default:
      return;
  }
  const GstStructure* s = gst_message_get_structure(msg);
  if (!s) return;
  guint fragmentId = 0;
  gst_structure_get_uint(s, "fragment-id", &fragmentId);
  const gchar* location = gst_structure_get_string(s, "location");
  const QString path = location ? QString::fromUtf8(location) : QString();
  if (gst_structure_has_name(s, "splitmuxsink-fragment-opened")) {
    int64_t runningTime = 0;
    if (!readTime(s, "running-time", runningTime)) return;
    RecordingSegment seg;
    seg.id = newId();
    seg.cameraId = a.record.cameraId;
    seg.sessionId = a.record.sessionId;
    seg.path = path;
    seg.state = QStringLiteral("recording");
    seg.startPtsNs = runningTime;
    seg.startUtcMs = a.utcFor(runningTime);
    seg.createdUtcMs = utcNowMs();
    if (!store_.insertSegment(seg)) {
      finish(QStringLiteral("cannot store a segment: ") + store_.lastError());
      return;
    }
    a.openSegments.insert(fragmentId, seg.id);
  } else if (gst_structure_has_name(s, "splitmuxsink-fragment-closed")) {
    int64_t offset = 0, duration = 0, runningTime = 0;
    const bool hasOffset = readTime(s, "fragment-offset", offset);
    const bool hasDuration = readTime(s, "fragment-duration", duration);
    const bool hasEnd = readTime(s, "running-time", runningTime);
    const QString segId = a.openSegments.take(fragmentId);
    const std::optional<RecordingSegment> seg = segId.isEmpty() ? std::nullopt : store_.getSegment(segId);
    if (!seg) return;
    const int64_t endPts = hasOffset && hasDuration ? offset + duration : hasEnd ? runningTime : seg->startPtsNs + duration;
    const int64_t bytes = QFileInfo(path).size();
    if (!store_.finalizeSegment(segId, endPts, a.utcFor(endPts), bytes, utcNowMs())) {
      finish(QStringLiteral("cannot finalize a segment: ") + store_.lastError());
      return;
    }
    ++a.record.segments;
    a.record.bytes += bytes;
    updateProgress();
  }
}

void ImportManager::updateProgress() {
  if (!active_ || !active_->pipeline) return;
  gint64 position = 0;
  if (active_->record.durationNs > 0 && gst_element_query_position(active_->pipeline, GST_FORMAT_TIME, &position) && position > 0)
    active_->record.progress = std::min(0.99, static_cast<double>(position) / static_cast<double>(active_->record.durationNs));
  store_.updateImport(active_->record);
}

void ImportManager::finish(const QString& failure) {
  progressTimer_.stop();
  std::unique_ptr<Active> a = std::move(active_);
  ImportRecord& r = a->record;
  if (a->pipeline) {
    GstBus* bus = gst_element_get_bus(a->pipeline);
    gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
    gst_object_unref(bus);
    gst_element_set_state(a->pipeline, GST_STATE_NULL);
    gst_object_unref(a->pipeline);
    a->pipeline = nullptr;
  }
  const int64_t now = utcNowMs();
  r.finishedUtcMs = now;
  if (failure.isEmpty() && a->openSegments.isEmpty()) {
    r.state = QStringLiteral("done");
    r.progress = 1.0;
    int64_t endUtc = r.startUtcMs;
    for (const RecordingSegment& seg : store_.segmentsOverlapping(r.cameraId, r.startUtcMs, r.startUtcMs + r.durationNs / 1'000'000 + 1))
      if (seg.sessionId == r.sessionId) endUtc = std::max(endUtc, seg.endUtcMs);
    store_.endSession(r.sessionId, QStringLiteral("eos"), endUtc);
    qInfo("import %s: done, %d segments, %lld bytes", qPrintable(r.id), r.segments, static_cast<long long>(r.bytes));
  } else {
    r.state = QStringLiteral("failed");
    r.error = failure.isEmpty() ? QStringLiteral("a segment did not finalize") : failure;
    MarkedEvidence marked;
    if (!r.sessionId.isEmpty() && !store_.deleteSessionSegments(r.sessionId, QStringLiteral("import_failed"), now, &marked))
      qWarning("import %s: cannot delete its segments: %s", qPrintable(r.id), qPrintable(store_.lastError()));
    Store::removeEvidenceFiles(marked.thumbnails, config_.dataDir + QStringLiteral("/evidence"));
    Store::removeIndexFiles(marked.indexThumbnails, config_.dataDir + QStringLiteral("/index"));
    store_.purgeDeletedSegmentFiles(config_.recordingsDir, now, std::numeric_limits<int>::max());
    if (!r.sessionId.isEmpty()) store_.endSession(r.sessionId, QStringLiteral("error"), now);
    if (const int left = removeSessionFiles(r.cameraId, r.sessionId); left > 0)
      qWarning("import %s: removed %d fragment files with no segment row", qPrintable(r.id), left);
    qWarning("import %s: failed: %s", qPrintable(r.id), qPrintable(r.error));
  }
  r.bytes = std::max<int64_t>(r.bytes, 0);
  store_.updateImport(r);
  store_.appendAudit(QStringLiteral("import"), r.state == QLatin1String("done") ? QStringLiteral("import.done") : QStringLiteral("import.fail"),
                     r.id,
                     detailJson({{"camera_id", r.cameraId},
                                 {"session_id", r.sessionId},
                                 {"segments", r.segments},
                                 {"bytes", static_cast<double>(r.bytes)},
                                 {"error", r.error}}),
                     now);
  QMetaObject::invokeMethod(this, [this] { startNext(); }, Qt::QueuedConnection);
}

void ImportManager::stopAll() {
  queue_.clear();
  sizes_.clear();
  for (ImportRecord r : store_.listImports(std::numeric_limits<int>::max())) {
    if (r.state != QLatin1String("queued")) continue;
    r.state = QStringLiteral("failed");
    r.error = QStringLiteral("core_stopped");
    r.finishedUtcMs = utcNowMs();
    store_.updateImport(r);
  }
  if (active_) finish(QStringLiteral("core_stopped"));
}

}

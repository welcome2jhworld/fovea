#pragma once
#include "fovea/core/Analytics.h"
#include "fovea/core/Config.h"
#include "fovea/core/Index.h"
#include <QHash>
#include <QObject>
#include <QTimer>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

typedef struct _GstElement GstElement;
typedef struct _GstMessage GstMessage;

namespace fovea::core {

class Store;

// Copies H.264/H.265 MP4 or MKV files into a camera's recordings as
// "imported" footage: filesrc ! qtdemux|matroskademux ! parse ! splitmuxsink
// (matroskamux, cut at keyframes every segment_seconds of the camera), no
// re-encoding. Each import is its own StreamSession with capture_clock
// "imported", anchored so media time zero is start_utc_ms. One import runs at
// a time; the file probe and the remux run off the core thread, and every
// store write happens on it. A failed or interrupted import deletes the
// segments it wrote.
class ImportManager : public QObject {
  Q_OBJECT
public:
  using Done = std::function<void(std::optional<ImportRecord> record, const ServiceError& error)>;

  ImportManager(Store& store, CoreConfig config, QObject* parent = nullptr);
  ~ImportManager() override;

  // Imports a previous run left queued or running fail with error
  // "core_restart" and lose the segments they wrote, including fragments no
  // segment row points at.
  void recover();
  // Validates the request, probes the file on a pool thread and queues the
  // import; done runs on the core thread with the queued import or the error.
  void submit(const QString& path, const QString& cameraId, int64_t startUtcMs, Done done);
  // Fails every queued or running import (error "core_stopped").
  void stopAll();

private:
  // Removes <recordings>/<camera>/<session>_*.mkv. A fragment whose
  // "fragment-opened" message was never drained has no row, so deleting the
  // session's segments leaves it behind and startup recovery would adopt it as
  // footage of an import that failed.
  int removeSessionFiles(const QString& cameraId, const QString& sessionId);

  struct Mailbox;
  struct Active;

  void onProbed(const ImportRecord& pending, int width, int height, Done done);
  void startNext();
  void drain();
  void handle(GstMessage* msg);
  void updateProgress();
  void finish(const QString& error);

  Store& store_;
  CoreConfig config_;
  std::shared_ptr<Mailbox> mailbox_;
  std::deque<QString> queue_;
  std::unique_ptr<Active> active_;
  QHash<QString, std::pair<int, int>> sizes_;
  QTimer progressTimer_;
};

}

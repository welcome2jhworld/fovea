#pragma once
#include "fovea/Api.h"
#include "fovea/core/Config.h"
#include <QObject>
#include <memory>
#include <optional>

namespace fovea::core {

class Store;

// Decodes recorded segments into frame rings so the console displays playback
// through the same surface it uses for live video. One decode pipeline per
// open channel; channels are closed explicitly or when the process exits.
class PlaybackManager : public QObject {
  Q_OBJECT
public:
  PlaybackManager(Store& store, CoreConfig config, QObject* parent = nullptr);
  ~PlaybackManager() override;

  std::optional<PlaybackState> open(const QString& segmentId, QString* error);
  std::optional<PlaybackState> openAt(const QString& cameraId, int64_t atUtcMs, QString* error);
  std::optional<PlaybackState> state(const QString& id) const;
  bool play(const QString& id);
  bool pause(const QString& id);
  bool seek(const QString& id, int64_t ptsNs);
  bool setRate(const QString& id, double rate);
  bool close(const QString& id);
  void closeAll();
  int openCount() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  CoreConfig config_;
};

}

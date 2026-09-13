#pragma once
#include <QCache>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>

namespace fovea::ui {

class CoreClient;

// Evidence thumbnails (GET /v1/evidence/{id}/thumbnail), decoded once and kept
// scaled to the largest size a view paints. A missing thumbnail is retried later.
class ThumbnailCache : public QObject {
  Q_OBJECT
public:
  static constexpr int kMaxWidth = 680;
  static constexpr int kCacheKb = 64 * 1024;
  static constexpr qint64 kRetryMs = 5000;

  explicit ThumbnailCache(CoreClient& client, QObject* parent = nullptr);

  // The cached image, or a null image while it is fetched; thumbnailReady follows.
  QImage thumbnail(const QString& evidenceId);

signals:
  void thumbnailReady(const QString& evidenceId);

private:
  CoreClient& client_;
  QCache<QString, QImage> images_;
  QSet<QString> inFlight_;
  QHash<QString, qint64> failedAtMs_;
};

}

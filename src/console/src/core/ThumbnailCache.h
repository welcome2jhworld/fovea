#pragma once
#include <QCache>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

QT_FORWARD_DECLARE_CLASS(QNetworkReply)

namespace fovea::ui {

class CoreClient;

// Thumbnails fetched by id, decoded on a pool thread and kept scaled to the largest
// size a view paints. A missing thumbnail is retried later.
class ThumbnailCache : public QObject {
  Q_OBJECT
public:
  // Evidence: GET /v1/evidence/{id}/thumbnail. SearchRecord: GET /v1/search/thumbnails/{record_id}.
  enum class Source { Evidence, SearchRecord };

  static constexpr int kMaxWidth = 680;
  static constexpr int kCacheKb = 64 * 1024;
  static constexpr qint64 kRetryMs = 5000;

  ThumbnailCache(CoreClient& client, Source source, QObject* parent = nullptr);

  // The cached image, or a null image while it is fetched; thumbnailReady follows.
  QImage thumbnail(const QString& id);
  // Aborts the fetches still in flight (the results are no longer painted), so
  // the next query's requests do not queue behind them.
  void cancelPending();

signals:
  void thumbnailReady(const QString& id);

private:
  void failed(const QString& id);

  CoreClient& client_;
  Source source_;
  QCache<QString, QImage> images_;
  QSet<QString> inFlight_;
  QHash<QString, QPointer<QNetworkReply>> replies_;
  QHash<QString, qint64> failedAtMs_;
};

}

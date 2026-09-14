#include "core/ThumbnailCache.h"
#include "core/CoreClient.h"
#include <QDateTime>
#include <QFuture>
#include <QPromise>
#include <QNetworkReply>
#include <QThreadPool>
#include <algorithm>
#include <memory>

namespace fovea::ui {

namespace {
QFuture<QImage> decodeJpeg(QByteArray body, int maxWidth) {
  auto promise = std::make_shared<QPromise<QImage>>();
  QFuture<QImage> future = promise->future();
  promise->start();
  QThreadPool::globalInstance()->start([promise, body = std::move(body), maxWidth] {
    QImage image;
    image.loadFromData(body, "JPEG");
    if (image.width() > maxWidth) image = image.scaledToWidth(maxWidth, Qt::SmoothTransformation);
    promise->addResult(std::move(image));
    promise->finish();
  });
  return future;
}
}

ThumbnailCache::ThumbnailCache(CoreClient& client, Source source, QObject* parent)
    : QObject(parent), client_(client), source_(source) {
  images_.setMaxCost(kCacheKb);
}

QImage ThumbnailCache::thumbnail(const QString& id) {
  if (id.isEmpty()) return {};
  if (const QImage* cached = images_.object(id)) return *cached;
  if (inFlight_.contains(id)) return {};
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const auto failedAt = failedAtMs_.constFind(id);
  if (failedAt != failedAtMs_.constEnd() && now - failedAt.value() < kRetryMs) return {};
  inFlight_.insert(id);
  auto onBody = [this, id](bool ok, const QByteArray& body, const QString&) {
    if (!ok || body.isEmpty()) return failed(id);
    decodeJpeg(body, kMaxWidth).then(this, [this, id](const QImage& image) {
      if (image.isNull()) return failed(id);
      inFlight_.remove(id);
      replies_.remove(id);
      failedAtMs_.remove(id);
      const int costKb = std::max<int>(1, static_cast<int>(image.sizeInBytes() / 1024));
      images_.insert(id, new QImage(image), costKb);
      emit thumbnailReady(id);
    });
  };
  if (source_ == Source::Evidence) client_.evidenceThumbnail(id, std::move(onBody), this);
  else replies_.insert(id, client_.searchThumbnail(id, std::move(onBody), this));
  return {};
}

void ThumbnailCache::cancelPending() {
  const QList<QPointer<QNetworkReply>> replies = replies_.values();
  replies_.clear();
  inFlight_.clear();
  for (const QPointer<QNetworkReply>& reply : replies)
    if (reply) reply->abort();
}

void ThumbnailCache::failed(const QString& id) {
  inFlight_.remove(id);
  replies_.remove(id);
  failedAtMs_.insert(id, QDateTime::currentMSecsSinceEpoch());
}

}

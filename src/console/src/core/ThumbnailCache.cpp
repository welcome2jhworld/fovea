#include "core/ThumbnailCache.h"
#include "core/CoreClient.h"
#include <QDateTime>
#include <algorithm>

namespace fovea::ui {

ThumbnailCache::ThumbnailCache(CoreClient& client, QObject* parent) : QObject(parent), client_(client) {
  images_.setMaxCost(kCacheKb);
}

QImage ThumbnailCache::thumbnail(const QString& evidenceId) {
  if (evidenceId.isEmpty()) return {};
  if (const QImage* cached = images_.object(evidenceId)) return *cached;
  if (inFlight_.contains(evidenceId)) return {};
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const auto failed = failedAtMs_.constFind(evidenceId);
  if (failed != failedAtMs_.constEnd() && now - failed.value() < kRetryMs) return {};
  inFlight_.insert(evidenceId);
  client_.evidenceThumbnail(evidenceId, [this, evidenceId](bool ok, const QByteArray& body, const QString&) {
    inFlight_.remove(evidenceId);
    QImage image;
    if (ok) image.loadFromData(body, "JPEG");
    if (image.isNull()) {
      failedAtMs_.insert(evidenceId, QDateTime::currentMSecsSinceEpoch());
      return;
    }
    failedAtMs_.remove(evidenceId);
    if (image.width() > kMaxWidth) image = image.scaledToWidth(kMaxWidth, Qt::SmoothTransformation);
    const int costKb = std::max<int>(1, static_cast<int>(image.sizeInBytes() / 1024));
    images_.insert(evidenceId, new QImage(std::move(image)), costKb);
    emit thumbnailReady(evidenceId);
  }, this);
  return {};
}

}

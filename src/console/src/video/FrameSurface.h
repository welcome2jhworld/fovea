#pragma once
#include "fovea/Api.h"
#include "fovea/FrameRing.h"
#include <QImage>
#include <QRectF>
#include <QString>
#include <QWidget>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace fovea::ui {

// Paints the newest frame of a shared-memory ring. Reads happen on the shared
// WallTicker; nothing here decodes or scales beyond QPainter::drawImage.
class FrameSurface : public QWidget {
  Q_OBJECT
public:
  explicit FrameSurface(QWidget* parent = nullptr);
  ~FrameSurface() override;

  // `session` identifies the writer run: the service re-creates a camera's ring under the
  // same name on every reconnect, so a new session forces a reopen (the last frame stays up).
  // A different ring drops the frame on screen.
  void bind(const std::optional<fovea::RingRef>& ring, const QString& session = {});
  void setCaption(const QString& caption);
  // Painted while no ring frame is shown (an evidence thumbnail before its clip plays).
  void setStillImage(const QImage& image);
  // Drops the frame on screen, e.g. when the surface is rebound to another clip.
  void clearFrame();
  void setCornerRadius(int radius);
  void setStripeOpacity(double opacity);

  bool hasFrame() const { return !image_.isNull(); }
  QSize frameSize() const { return image_.size(); }
  // Where the frame (or still image) is painted inside the widget, letterboxed; empty without either.
  QRectF imageRect() const;
  // Media time and session of the frame on screen, from its ring header.
  int64_t framePtsNs() const { return static_cast<int64_t>(frameHeader_.ptsNs); }
  const std::array<uint8_t, 16>& frameSession() const { return frameHeader_.sessionId; }
  // The process that wrote the bound ring has exited.
  bool writerLost() const { return writerLost_; }
  // When this console last saw a new frame in the ring (monoNowNs clock); 0 before the first.
  int64_t lastFrameMonoNs() const { return lastFrameMonoNs_; }

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  static constexpr int64_t kOpenRetryNs = 500'000'000;
  static constexpr int64_t kWriterCheckNs = 1'000'000'000;

  void onTick();
  bool openReader(int64_t nowMonoNs);

  QString ringName_;
  QString session_;
  std::unique_ptr<fovea::FrameRingReader> reader_;
  // Frames are copied into staging_ and swapped into display_ only when the copy was not torn.
  std::vector<uint8_t> staging_;
  std::vector<uint8_t> display_;
  QImage image_;
  QImage still_;
  fovea::FrameHeader frameHeader_;
  uint64_t lastSeenSeq_ = 0;
  uint64_t lastObservedSeq_ = 0;
  int64_t lastOpenAttemptNs_ = 0;
  int64_t lastWriterCheckNs_ = 0;
  int64_t lastFrameMonoNs_ = 0;
  bool writerLost_ = false;
  QString caption_;
  int cornerRadius_ = 0;
  double stripeOpacity_ = 1.0;
};

}

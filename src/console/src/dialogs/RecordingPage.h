#pragma once
#include <QWidget>

class QSpinBox;

namespace fovea::ui {

// Provisional: the handoff has no Recording tab design; only the segment length is real in M1.
class RecordingPage : public QWidget {
  Q_OBJECT
public:
  explicit RecordingPage(QWidget* parent = nullptr);
  void setSegmentSeconds(int seconds);
  int segmentSeconds() const;

private:
  QSpinBox* segment_ = nullptr;
};

}

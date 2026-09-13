#pragma once
#include "core/CoreLauncher.h"
#include <QWidget>

class QLabel;
class QPushButton;

namespace fovea::ui {

// Thin line above the wall while the core is not ready.
class ServiceStatusLine : public QWidget {
  Q_OBJECT
public:
  static constexpr int kHeight = 28;
  explicit ServiceStatusLine(QWidget* parent = nullptr);
  void setState(CoreLauncher::State state, const QString& message);

signals:
  void retryRequested();

private:
  QLabel* text_ = nullptr;
  QPushButton* retry_ = nullptr;
};

}

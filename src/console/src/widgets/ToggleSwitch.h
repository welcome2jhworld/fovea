#pragma once
#include <QAbstractButton>
#include <QVariantAnimation>

namespace fovea::ui {

// 34x18 (md) or 30x16 (sm) switch; QSS cannot slide a knob so it is painted.
class ToggleSwitch : public QAbstractButton {
  Q_OBJECT
public:
  enum class Size { Md, Sm };
  explicit ToggleSwitch(Size size = Size::Md, QWidget* parent = nullptr);
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;
  void checkStateSet() override;
  void nextCheckState() override;

private:
  void animateTo(bool on);
  Size size_;
  QVariantAnimation slide_;
  double knob_ = 0.0;
};

}

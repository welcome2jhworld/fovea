#pragma once
#include "search/SearchLogic.h"
#include <QWidget>

class QDateTimeEdit;
class QLabel;

namespace fovea::ui {

// "Custom range" popup under the time chip: From and To in local time, Apply checks From < To.
// Enter applies, Esc closes.
class CustomRangePopup : public QWidget {
  Q_OBJECT
public:
  static constexpr int kWidth = 300;

  explicit CustomRangePopup(QWidget* parent = nullptr);
  void showBelow(QWidget* anchor, const TimeRange& initial);

signals:
  void applied(const TimeRange& range);
  void closed();

protected:
  void hideEvent(QHideEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

private:
  void apply();

  QDateTimeEdit* from_ = nullptr;
  QDateTimeEdit* to_ = nullptr;
  QLabel* error_ = nullptr;
};

}

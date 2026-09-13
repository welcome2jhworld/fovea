#pragma once
#include <QWidget>

namespace fovea::ui {

class NotImplementedState : public QWidget {
  Q_OBJECT
public:
  NotImplementedState(const QString& sentence, const QString& milestone, QWidget* parent = nullptr);
};

}

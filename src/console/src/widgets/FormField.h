#pragma once
#include <QLabel>
#include <QString>
#include <QWidget>

namespace fovea::ui {

// Label (12 px muted) over a control, 6 px apart; optional trailing hint on the label row.
QWidget* formField(const QString& label, QWidget* control, QWidget* parent, const QString& hint = {},
                   QWidget* labelAction = nullptr);
QLabel* fieldLabel(const QString& text, QWidget* parent);
QLabel* hintLabel(const QString& text, QWidget* parent);
void restyle(QWidget* widget);

}

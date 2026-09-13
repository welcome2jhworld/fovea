#include "widgets/FormField.h"
#include <QHBoxLayout>
#include <QStyle>
#include <QVBoxLayout>

namespace fovea::ui {

namespace {
constexpr int kLabelGap = 6;
}

QLabel* fieldLabel(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", QStringLiteral("field-label"));
  return label;
}

QLabel* hintLabel(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", QStringLiteral("hint"));
  return label;
}

void restyle(QWidget* widget) {
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
}

QWidget* formField(const QString& label, QWidget* control, QWidget* parent, const QString& hint,
                   QWidget* labelAction) {
  auto* field = new QWidget(parent);
  auto* column = new QVBoxLayout(field);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(kLabelGap);
  auto* labelRow = new QHBoxLayout();
  labelRow->setContentsMargins(0, 0, 0, 0);
  labelRow->setSpacing(6);
  labelRow->addWidget(fieldLabel(label, field));
  if (!hint.isEmpty()) labelRow->addWidget(hintLabel(hint, field));
  labelRow->addStretch(1);
  if (labelAction) {
    labelAction->setParent(field);
    labelRow->addWidget(labelAction);
  }
  column->addLayout(labelRow);
  control->setParent(field);
  column->addWidget(control);
  return field;
}

}

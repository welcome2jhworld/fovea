#include "screens/NotImplementedState.h"
#include "theme/Theme.h"
#include <QLabel>
#include <QVBoxLayout>

namespace fovea::ui {

NotImplementedState::NotImplementedState(const QString& sentence, const QString& milestone, QWidget* parent)
    : QWidget(parent) {
  setObjectName(QStringLiteral("NotImplementedState"));
  setAttribute(Qt::WA_StyledBackground, true);
  auto* text = new QLabel(sentence, this);
  text->setObjectName(QStringLiteral("NotImplementedSentence"));
  text->setAlignment(Qt::AlignCenter);
  auto* tag = new QLabel(milestone, this);
  tag->setObjectName(QStringLiteral("NotImplementedMilestone"));
  tag->setFont(Theme::monoLabel());
  tag->setAlignment(Qt::AlignCenter);
  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(6);
  column->addStretch(1);
  column->addWidget(text);
  column->addWidget(tag);
  column->addStretch(1);
}

}

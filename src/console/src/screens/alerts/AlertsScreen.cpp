#include "screens/alerts/AlertsScreen.h"
#include "screens/NotImplementedState.h"
#include "screens/alerts/AlertDetailPanel.h"
#include "screens/alerts/AlertTable.h"
#include "screens/alerts/RulesRail.h"
#include "theme/Tokens.h"
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace fovea::ui {
namespace tk = tokens;

AlertsScreen::AlertsScreen(CoreClient& client, EventStore& store, ThumbnailCache& thumbnails, QWidget* parent)
    : QWidget(parent) {
  setObjectName(QStringLiteral("AlertsScreen"));

  auto* subTabs = new QWidget(this);
  subTabs->setObjectName(QStringLiteral("SubTabBar"));
  subTabs->setAttribute(Qt::WA_StyledBackground, true);
  subTabs->setFixedHeight(tk::size::subTabBar);
  auto* group = new QButtonGroup(this);
  auto* subTabRow = new QHBoxLayout(subTabs);
  subTabRow->setContentsMargins(tk::size::tabBarPaddingX, 0, tk::size::tabBarPaddingX, 0);
  subTabRow->setSpacing(6);
  const char* const labels[] = {"Alert log", "Statistics"};
  for (int i = 0; i < 2; ++i) {
    auto* tab = new QToolButton(subTabs);
    tab->setText(QString::fromLatin1(labels[i]));
    tab->setProperty("role", QStringLiteral("subtab"));
    tab->setCheckable(true);
    tab->setCursor(Qt::PointingHandCursor);
    tab->setFocusPolicy(Qt::NoFocus);
    group->addButton(tab, i);
    subTabRow->addWidget(tab);
    if (i == 0) alertLogTab_ = tab;
  }
  subTabRow->addStretch(1);
  alertLogTab_->setChecked(true);

  auto* log = new QWidget(this);
  rules_ = new RulesRail(client, store, log);
  table_ = new AlertTable(store, thumbnails, log);
  detail_ = new AlertDetailPanel(client, store, thumbnails, log);
  auto* logRow = new QHBoxLayout(log);
  logRow->setContentsMargins(0, 0, 0, 0);
  logRow->setSpacing(0);
  logRow->addWidget(rules_);
  logRow->addWidget(table_, 1);
  logRow->addWidget(detail_);

  pages_ = new QStackedWidget(this);
  pages_->addWidget(log);
  pages_->addWidget(new NotImplementedState(QStringLiteral("Statistics are not implemented in this build."),
                                            QStringLiteral("NOT IN M3"), pages_));

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(subTabs);
  column->addWidget(pages_, 1);

  connect(group, &QButtonGroup::idClicked, pages_, &QStackedWidget::setCurrentIndex);
  connect(table_, &AlertTable::selectedEventChanged, detail_, &AlertDetailPanel::setEventId);
}

void AlertsScreen::setSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses) {
  rules_->setCameras(cameras, statuses);
  table_->setCameras(cameras);
  detail_->setCameras(cameras);
}

void AlertsScreen::showAlertLog() {
  alertLogTab_->setChecked(true);
  pages_->setCurrentIndex(0);
}

void AlertsScreen::openEvent(const QString& eventId) {
  showAlertLog();
  table_->selectEvent(eventId);
}

}

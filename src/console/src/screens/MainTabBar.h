#pragma once
#include <QStringList>
#include <QVector>
#include <QWidget>

namespace fovea::ui {

class NavTabs : public QWidget {
  Q_OBJECT
public:
  explicit NavTabs(const QStringList& labels, QWidget* parent = nullptr);
  int currentIndex() const { return current_; }
  int count() const { return static_cast<int>(labels_.size()); }
  void setCurrentIndex(int index);
  void setBadge(int index, int count);

signals:
  void currentChanged(int index);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;

private:
  int tabWidth(int index) const;
  int badgeWidth(int index) const;

  QStringList labels_;
  QVector<int> badges_;
  int current_ = 0;
};

class MainTabBar : public QWidget {
  Q_OBJECT
public:
  explicit MainTabBar(QWidget* parent = nullptr);
  NavTabs* tabs() const { return tabs_; }
  QWidget* rightCluster() const { return right_; }

private:
  NavTabs* tabs_ = nullptr;
  QWidget* right_ = nullptr;
};

}

#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

namespace fovea::ui {

class CoreClient;

class CoreLauncher : public QObject {
  Q_OBJECT
public:
  enum class State { Unknown, Starting, Ready, Failed };
  Q_ENUM(State)

  static constexpr int kPollIntervalMs = 500;
  static constexpr int kStartupTimeoutMs = 15000;

  CoreLauncher(CoreClient& client, QObject* parent = nullptr);

  State state() const { return state_; }
  QString message() const { return message_; }
  QString coreBinary() const;

  void start();
  // Re-run discovery when the running service stops answering.
  void recheck();
  void stopService();

signals:
  void stateChanged(State state, const QString& message);

private:
  void setState(State state, const QString& message);
  void probe(bool launchIfDown);
  void launch();
  void pollUntilReady();

  CoreClient& client_;
  State state_ = State::Unknown;
  QString message_;
  QTimer pollTimer_;
  QElapsedTimer startedAt_;
  qint64 launchedPid_ = 0;
  bool probing_ = false;
  // Whether the pending probe may launch a service; start() during a probe upgrades it.
  bool launchIfDown_ = false;
};

}

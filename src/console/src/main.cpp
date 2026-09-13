#include "screens/MainWindow.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QScreen>
#include <QTimer>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("Fovea"));
  QCoreApplication::setApplicationName(QStringLiteral("Fovea"));
  QCoreApplication::setApplicationVersion(QStringLiteral(FOVEA_VERSION));

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("Fovea desktop console"));
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption dataDir(QStringLiteral("data-dir"), QStringLiteral("Data directory shared with fovea-core."),
                                   QStringLiteral("path"));
  parser.addOption(dataDir);
  parser.process(app);
  if (parser.isSet(dataDir)) qputenv("FOVEA_DATA_DIR", parser.value(dataDir).toLocal8Bit());

  if (!fovea::ui::Theme::loadFonts()) qWarning() << "bundled fonts failed to load; using system fonts";
  qInfo().noquote() << "fonts:" << fovea::ui::Theme::fontReport();
  app.setFont(fovea::ui::Theme::body());
  app.setStyleSheet(fovea::ui::Theme::styleSheet());

  fovea::ui::MainWindow window;
  window.setScreenshotView(QString::fromLocal8Bit(qgetenv("FOVEA_SCREENSHOT_VIEW")));
  QSize initial(fovea::ui::tokens::size::windowDesignWidth, fovea::ui::tokens::size::windowDesignHeight);
  if (QScreen* screen = app.primaryScreen()) initial = initial.boundedTo(screen->availableSize());
  window.resize(initial.expandedTo(window.minimumSize()));
  window.show();

  const QString screenshot = QString::fromLocal8Bit(qgetenv("FOVEA_SCREENSHOT"));
  if (!screenshot.isEmpty()) {
    bool delayOk = false;
    int delayMs = qEnvironmentVariableIntValue("FOVEA_SCREENSHOT_DELAY_MS", &delayOk);
    if (!delayOk || delayMs <= 0) delayMs = 4000;
    QTimer::singleShot(delayMs, &window, [&window, screenshot] {
      const bool saved = window.grab().save(screenshot);
      if (!saved) qWarning() << "could not save screenshot to" << screenshot;
      window.close();
      QCoreApplication::exit(saved ? 0 : 1);
    });
  }
  return app.exec();
}

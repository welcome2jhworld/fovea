#pragma once
#include <QFont>
#include <QString>

class QWidget;

namespace fovea::ui {

class Theme {
public:
  static bool loadFonts();
  static QString fontReport();
  static QString styleSheet();

  // Size class behind the [variant="…"] selectors (xs, sm, md, lg for buttons; sm for inputs).
  // QWidget already declares a `size` property, so the dynamic property has its own name.
  static void setVariant(QWidget* widget, const char* variant);

  static QFont sans(int px, QFont::Weight weight = QFont::Normal);
  static QFont mono(int px);
  static QFont display();
  static QFont heading();
  static QFont subhead();
  static QFont body();
  static QFont small();
  static QFont monoLabel();
  static QFont monoMicro();
  static QFont placeholderCaption(int surfaceHeight);
};

}

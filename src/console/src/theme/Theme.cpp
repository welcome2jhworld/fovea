#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QFontDatabase>
#include <QMetaObject>
#include <QStringList>
#include <QWidget>

namespace fovea::ui {
namespace tk = tokens;

namespace {
constexpr const char* kVariantProperty = "variant";

QFont withSpacing(QFont f, double px) {
  f.setLetterSpacing(QFont::AbsoluteSpacing, px);
  return f;
}

// Every button carries a 1px border (transparent on filled and ghost roles), and
// QSS heights are content-box, so each variant is emitted 2px under its token.
QString buttonSizes() {
  struct Variant { const char* name; int height; int padX; int radius; };
  const Variant variants[] = {{"xs", tk::size::buttonXs, 9, tk::radius::control},
                              {"sm", tk::size::buttonSm, 12, tk::radius::control},
                              {"md", tk::size::buttonMd, 12, tk::radius::input},
                              {"lg", tk::size::buttonLg, 16, tk::radius::input}};
  QString out;
  for (const Variant& v : variants) {
    out += QStringLiteral("QPushButton[variant=\"%1\"] { min-height: %2px; max-height: %2px; padding: 0 %3px; "
                          "border-radius: %4px; }\n")
               .arg(QLatin1StringView(v.name)).arg(v.height - 2).arg(v.padX).arg(v.radius);
  }
  return out;
}
}

bool Theme::loadFonts() {
  const char* files[] = {":/fonts/IBMPlexSans-Regular.ttf", ":/fonts/IBMPlexSans-Medium.ttf",
                         ":/fonts/IBMPlexSans-SemiBold.ttf", ":/fonts/IBMPlexMono-Regular.ttf"};
  bool ok = true;
  for (const char* f : files) ok = QFontDatabase::addApplicationFont(QLatin1StringView(f)) >= 0 && ok;
  return ok;
}

QString Theme::fontReport() {
  const QString sansStyles = QFontDatabase::styles(QString(tk::font::sans)).join(QStringLiteral(", "));
  const QString monoStyles = QFontDatabase::styles(QString(tk::font::mono)).join(QStringLiteral(", "));
  return QStringLiteral("%1 [%2]; %3 [%4]").arg(tk::font::sans, sansStyles, tk::font::mono, monoStyles);
}

void Theme::setVariant(QWidget* widget, const char* variant) {
  Q_ASSERT(widget->metaObject()->indexOfProperty(kVariantProperty) < 0);
  widget->setProperty(kVariantProperty, QString::fromLatin1(variant));
}

QFont Theme::sans(int px, QFont::Weight weight) {
  QFont f{QString(tk::font::sans)};
  f.setPixelSize(px);
  f.setWeight(weight);
  return f;
}

QFont Theme::mono(int px) {
  QFont f{QString(tk::font::mono)};
  f.setPixelSize(px);
  f.setWeight(QFont::Normal);
  return f;
}

QFont Theme::display() { return withSpacing(sans(tk::font::display, QFont::DemiBold), tk::font::displaySpacingPx); }
QFont Theme::heading() { return sans(tk::font::heading, QFont::DemiBold); }
QFont Theme::subhead() { return sans(tk::font::subhead, QFont::DemiBold); }
QFont Theme::body() { return sans(tk::font::body); }
QFont Theme::small() { return sans(tk::font::small); }
QFont Theme::monoLabel() { return withSpacing(mono(tk::font::label), tk::font::labelSpacingPx); }
QFont Theme::monoMicro() { return mono(tk::font::micro); }

QFont Theme::placeholderCaption(int surfaceHeight) {
  return surfaceHeight >= 190 ? withSpacing(mono(tk::font::label), tk::font::captionSpacingPx)
                              : withSpacing(mono(tk::font::micro), tk::font::captionMicroSpacingPx);
}

// The application font (Theme::body) is the only universal font: a `QWidget`
// font rule would override every QWidget::setFont in the console. Heights of
// bordered controls are content-box, hence token minus 2.
QString Theme::styleSheet() {
  using namespace tk::color;
  const QString monoFamily = QStringLiteral("\"%1\"").arg(tk::font::mono);
  const QString sansFamily = QStringLiteral("\"%1\"").arg(tk::font::sans);
  QString qss = QStringLiteral(R"(
QWidget#AppRoot { background: %bgApp%; }
QWidget { color: %textPrimary%; }

#TitleBar { background: %bgDeep%; border-bottom: 1px solid %lineQuiet%; }
#MainTabBar { background: %bgApp%; border-bottom: 1px solid %lineQuiet%; }
#WallToolbar { background: %bgApp%; border-bottom: 1px solid %lineQuiet%; }
#RailHeader { background: %bgApp%; border-bottom: 1px solid %lineQuiet%; }
#CameraRail { background: %bgApp%; border-right: 1px solid %lineQuiet%; }
#WallArea, #VideoWall { background: %bgDeep%; }
#ServiceStatusLine { background: %bgPanel%; border-bottom: 1px solid %lineQuiet%; }
#ServiceStatusLine QLabel { font-size: 12px; color: %textSecondary%; }
#ServiceStatusLine QLabel[tone="critical"] { color: %critical%; }
#ServiceStatusLine QLabel[tone="positive"] { color: %positive%; }
#NotImplementedState { background: %bgApp%; }
#NotImplementedSentence { color: %textSecondary%; }
#NotImplementedMilestone { color: %textDisabled%; }
#ScreenStack { background: %bgApp%; }

QWidget#BrandMark { background: %accent%; border-radius: 4px; }
QToolButton#BrandName { background: transparent; border: none; padding: 0; color: %textPrimary%; }
QToolButton#BrandName::menu-indicator { image: none; width: 0; }
QToolButton#WinMinimize, QToolButton#WinMaximize, QToolButton#WinClose {
  background: transparent; border: none; padding: 0; color: %textMuted%; }

QLabel[role="section-label"] { color: %textMuted%; }
QLabel[role="field-label"] { font-size: 12px; color: %textMuted%; }
QLabel[role="hint"] { font-size: 12px; color: %textDisabled%; }
QLabel[tone="critical"] { color: %critical%; }
QLabel[tone="muted"] { color: %textMuted%; }
QLabel[role="metric-key"] { font-size: 12px; color: %textMuted%; }
QLabel[role="metric-value"] { color: %textPrimary%; }

QPushButton { background: %bgRaised%; border: 1px solid %lineStrong%; border-radius: 6px; color: %textPrimary%;
  padding: 0 16px; min-height: 30px; max-height: 30px; }
QPushButton[role="primary"] { background: %accent%; color: %accentInk%; font-weight: 600; border: 1px solid transparent; }
QPushButton[role="secondary"] { background: %bgRaised%; border: 1px solid %lineStrong%; color: %textPrimary%; }
QPushButton[role="secondary"][variant="xs"] { background: %bgPanel%; border: 1px solid %line%; font-size: 12px; }
QPushButton[role="ghost"] { background: transparent; border: 1px solid transparent; color: %textSecondary%; }
QPushButton[role="destructive-ghost"] { background: transparent; border: 1px solid transparent; color: %critical%; }
QPushButton[role="link"] { background: transparent; border: none; font-size: 12px; color: %accent%; padding: 0;
  min-height: 0; max-height: 32px; }
QPushButton:disabled { background: %bgPanel%; border: 1px solid %line%; color: %textDisabled%; }
%buttonSizes%
QPushButton#RemoveCamera { padding: 0 12px; }
QPushButton#SaveCamera { padding: 0 18px; }

QToolButton { background: transparent; border: none; color: %textSecondary%; }
QToolButton#CameraSettingsButton { min-width: 22px; max-width: 22px; min-height: 22px; max-height: 22px; padding: 0;
  border-radius: 5px; background: %bgPanel%; border: 1px solid %line%; }

QLineEdit, QComboBox { min-height: 32px; max-height: 32px; border-radius: 6px; background: %bgPanel%;
  border: 1px solid %line%; padding: 0 12px; color: %textPrimary%;
  selection-background-color: %accent%; selection-color: %accentInk%; placeholder-text-color: %textMuted%; }
QLineEdit:focus, QComboBox:focus { border: 1px solid %accent%; }
QLineEdit[mono="true"] { font-family: %mono%; font-size: 12px; }
QLineEdit#SubUrl { color: %textSecondary%; }
QLineEdit[variant="sm"] { min-height: 28px; max-height: 28px; font-size: 12px; padding: 0 10px; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox::down-arrow { image: url(:/icons/combo-caret.svg); width: 8px; height: 8px; }
QComboBox QLineEdit, QComboBox QLineEdit:focus { border: none; background: transparent; padding: 0;
  min-height: 30px; max-height: 30px; }
QComboBox QAbstractItemView { background: %bgPanel%; border: 1px solid %lineStrong%; border-radius: 6px; padding: 4px;
  outline: 0; color: %textSecondary%; selection-background-color: %bgRaised%; selection-color: %textPrimary%; }
QSpinBox, QTimeEdit { min-height: 32px; max-height: 32px; border-radius: 6px; background: %bgPanel%; border: 1px solid %line%;
  padding: 0 12px; color: %textPrimary%; selection-background-color: %accent%; selection-color: %accentInk%; }
QSpinBox:focus, QTimeEdit:focus { border: 1px solid %accent%; }
QSpinBox::up-button, QSpinBox::down-button, QTimeEdit::up-button, QTimeEdit::down-button { width: 0; border: none; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QTimeEdit:disabled { color: %textDisabled%; }
QLineEdit[surface="app"], QComboBox[surface="app"], QSpinBox[surface="app"], QTimeEdit[surface="app"] {
  background: %bgApp%; min-height: 30px; max-height: 30px; padding: 0 10px; }
QComboBox[surface="app"] QLineEdit { min-height: 28px; max-height: 28px; }
QComboBox[tone="critical"] { color: %critical%; }
QComboBox[tone="review"] { color: %warning%; }
QComboBox[tone="info"] { color: %accent%; }
QComboBox[variant="sm"] { min-height: 26px; max-height: 26px; border-radius: 5px; padding: 0 12px; }

QTreeView#CameraTree { background: transparent; border: none; outline: 0; show-decoration-selected: 0;
  selection-background-color: transparent; }
QTreeView#CameraTree::item { background: transparent; border: none; padding: 0; }
QTreeView#CameraTree::item:selected, QTreeView#CameraTree::item:hover { background: transparent; }
QTreeView#CameraTree::branch { background: transparent; image: none; border-image: none; }

#LayoutPopupFrame { background: %bgPanel%; border: 1px solid %lineStrong%; border-radius: 8px; }
QLabel#PopupSectionLabel { color: %textMuted%; }
QLabel#PopupRowLabel { color: %textSecondary%; }
QFrame#PopupDivider { background: %line%; min-height: 1px; max-height: 1px; border: none; }

QFrame[role="card"] { background: %bgPanel%; border: 1px solid %line%; border-radius: 8px; }

QMenu { background: %bgPanel%; border: 1px solid %lineStrong%; border-radius: 8px; padding: 6px; }
QMenu::item { min-height: 32px; padding: 0 12px; border-radius: 5px; color: %textSecondary%; }
QMenu::item:selected { background: %bgRaised%; color: %textPrimary%; }
QMenu::separator { height: 1px; background: %line%; margin: 6px 4px; }

QMessageBox { background: %bgApp%; }
QMessageBox QLabel { color: %textPrimary%; }

QScrollBar:vertical { background: transparent; width: 6px; margin: 0; }
QScrollBar::handle:vertical { background: %lineStrong%; border-radius: 3px; min-height: 24px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QScrollBar:horizontal { background: transparent; height: 6px; margin: 0; }
QScrollBar::handle:horizontal { background: %lineStrong%; border-radius: 3px; min-width: 24px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }

QToolTip { background: %bgPanel%; border: 1px solid %lineStrong%; color: %textPrimary%; font-size: 12px; padding: 4px 8px; }

#DialogHeader { border-bottom: 1px solid %lineQuiet%; }
#DialogFooter { border-top: 1px solid %lineQuiet%; }
QLabel#DialogTitle { color: %textPrimary%; }
QLabel#DialogSubtitle { color: %textMuted%; }
QLabel#DialogError { font-size: 12px; }
QToolButton#DialogClose { background: transparent; border: none; padding: 0 2px; }
QListWidget#DialogTabList { background: transparent; border: none; border-right: 1px solid %lineQuiet%; outline: 0;
  padding: 12px 8px; }
QListWidget#DialogTabList::item { padding: 0 12px; border-radius: 6px; color: %textSecondary%; }
QListWidget#DialogTabList::item:hover { background: transparent; }
QListWidget#DialogTabList::item:selected { background: %bgRaised%; color: %textPrimary%; font-weight: 500; }
QFrame#TestFrame { background: %bgVideo%; border: 1px solid %line%; border-radius: 6px; }
QFrame#PasswordField { min-height: 32px; max-height: 32px; border-radius: 6px; background: %bgPanel%; border: 1px solid %line%; }
QFrame#PasswordField[focus="true"] { border: 1px solid %accent%; }
QFrame#PasswordField QLineEdit, QFrame#PasswordField QLineEdit:focus { border: none; background: transparent;
  border-radius: 0; min-height: 32px; max-height: 32px; padding: 0 0 0 11px;
  font-family: %mono%; font-size: 13px; color: %textSecondary%; }
QToolButton#RevealPassword { background: transparent; border: none; color: %textMuted%; padding: 0 2px; }

QToolButton#OverlaysButton, QToolButton#RecordingsButton { min-height: 28px; max-height: 28px; padding: 0 12px;
  border-radius: 6px; background: %bgPanel%; border: 1px solid %line%; color: %textSecondary%; font-size: 13px; }
QToolButton#OverlaysButton:checked, QToolButton#RecordingsButton:checked { background: %bgRaised%;
  border: 1px solid %lineStrong%; color: %textPrimary%; }

#LiveEventFeed, #AlertDetail { background: %bgApp%; border-left: 1px solid %lineQuiet%; }
#FeedHeader, #RulesHeader, #AlertToolbar, #DetailHeader { background: %bgApp%; border-bottom: 1px solid %lineQuiet%; }
#FeedFooter, #RulesFooter { background: %bgApp%; border-top: 1px solid %lineQuiet%; }
QListView#EventList, QListView#AlertList { background: transparent; border: none; outline: 0; }
QListView#EventList::item, QListView#AlertList::item { background: transparent; border: none; padding: 0; }
QListView#EventList::item:hover, QListView#EventList::item:selected,
QListView#AlertList::item:hover, QListView#AlertList::item:selected { background: transparent; }
QLabel#FeedEmpty, QLabel#AlertEmpty, QLabel#RulesEmpty, QLabel#DetailEmpty { color: %textSecondary%; }

#AlertsScreen, #AlertTable { background: %bgApp%; }
#SubTabBar { background: %bgApp%; border-bottom: 1px solid %lineQuiet%; }
QToolButton[role="subtab"], QToolButton[role="status-tab"] { min-height: 26px; max-height: 26px; border-radius: 5px;
  background: transparent; border: 1px solid transparent; color: %textSecondary%; font-size: 13px; }
QToolButton[role="subtab"] { padding: 0 14px; }
QToolButton[role="status-tab"] { padding: 0 12px; }
QToolButton[role="subtab"]:checked, QToolButton[role="status-tab"]:checked { background: %bgRaised%;
  border: 1px solid %lineStrong%; color: %textPrimary%; }
QToolButton[role="subtab"]:checked { font-weight: 500; }

#RulesRail { background: %bgApp%; border-right: 1px solid %lineQuiet%; }
QScrollArea#RulesScroll, QScrollArea#DetailScroll { background: transparent; border: none; }
QWidget#RulesList, QWidget#DetailBody { background: transparent; }
QLabel#RulesCounts { color: %textDisabled%; }
QLabel#RulesRange { color: %textMuted%; }
QLabel#RuleCardName { font-weight: 600; color: %textPrimary%; }
QLabel#RuleSentence { font-size: 12px; color: %textSecondary%; }
QPushButton#RuleEdit { background: transparent; border: none; padding: 0; min-height: 0; max-height: 20px; font-size: 12px;
  color: %textMuted%; }
QPushButton#RuleEdit[editing="true"] { color: %accent%; }
QPushButton#RuleTest, QPushButton#RuleCancel { padding: 0 12px; }
QPushButton#RuleSave { padding: 0 14px; }
QLabel[role="editor-label"] { font-size: 11px; color: %textMuted%; }
QToolButton[role="day"] { background: %bgApp%; border: 1px solid %line%; border-radius: 4px; color: %textMuted%; padding: 0; }
QToolButton[role="day"]:checked { background: %bgRaised%; border: 1px solid %accent%; color: %textPrimary%; }
QToolButton[role="pager"] { background: transparent; border: 1px solid transparent; border-radius: 5px;
  color: %textSecondary%; font-family: %mono%; font-size: 12px; padding: 0; }
QToolButton[role="pager"][current="true"] { background: %bgRaised%; border: 1px solid %accent%; color: %textPrimary%; }
QToolButton[role="pager"][arrow="true"] { background: %bgPanel%; border: 1px solid %line%; color: %textPrimary%;
  font-family: %sans%; }
QToolButton[role="pager"]:disabled { color: %textDisabled%; }
QLabel#RuleEditorError, QLabel#DetailError { font-size: 12px; }

QWidget#DetailRule { background: %lineQuiet%; }
QLabel#DetailTitle { color: %textPrimary%; }
QLabel#DetailSecondary, QLabel#DetailActivity { font-size: 12px; color: %textSecondary%; }
QWidget#MetaRow { background: transparent; border-bottom: 1px solid %lineRow%; }
QLabel#MetaKey { font-size: 12px; color: %textMuted%; }
QLabel#MetaValue { color: %textPrimary%; }
QPushButton#ReviewButton:checked { background: %bgRaised%; border: 1px solid %accent%; color: %textPrimary%; }

#RecordingsPanel { background: %bgApp%; border-left: 1px solid %lineQuiet%; }
#RecordingsHeader { background: %bgApp%; border-bottom: 1px solid %lineQuiet%; }
QListView#RecordingsList { background: transparent; border: none; outline: 0; }
QListView#RecordingsList::item { background: transparent; border: none; padding: 0; }
QListView#RecordingsList::item:hover, QListView#RecordingsList::item:selected { background: transparent; }
QLabel#RecordingsMessage { color: %textSecondary%; }
)");
  const struct { const char* key; QString value; } subs[] = {
      {"%mono%", monoFamily}, {"%sans%", sansFamily}, {"%buttonSizes%", buttonSizes()},
      {"%bgDeep%", QString(bgDeep)}, {"%bgApp%", QString(bgApp)}, {"%bgPanel%", QString(bgPanel)},
      {"%bgRaised%", QString(bgRaised)}, {"%bgVideo%", QString(bgVideo)}, {"%line%", QString(line)},
      {"%lineStrong%", QString(lineStrong)}, {"%lineQuiet%", QString(lineQuiet)},
      {"%textPrimary%", QString(textPrimary)}, {"%textSecondary%", QString(textSecondary)},
      {"%textMuted%", QString(textMuted)}, {"%textDisabled%", QString(textDisabled)},
      {"%accent%", QString(accent)}, {"%accentInk%", QString(accentInk)}, {"%critical%", QString(critical)},
      {"%positive%", QString(positive)}, {"%warning%", QString(warning)}, {"%lineRow%", QString(lineRow)}};
  for (const auto& s : subs) qss.replace(QLatin1StringView(s.key), s.value);
  return qss;
}

}

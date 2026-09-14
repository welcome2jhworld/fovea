#pragma once
#include <QColor>
#include <QLatin1StringView>

namespace fovea::ui::tokens {

// Colour tokens (docs/design/handoff-map.md section 3a).
namespace color {
constexpr QLatin1StringView bgDeep{"#050607"};
constexpr QLatin1StringView bgApp{"#0A0B0C"};
constexpr QLatin1StringView bgPanel{"#121315"};
constexpr QLatin1StringView bgRaised{"#1A1C1F"};
constexpr QLatin1StringView bgVideo{"#0E0F11"};
constexpr QLatin1StringView line{"#26292D"};
constexpr QLatin1StringView lineStrong{"#34383D"};
constexpr QLatin1StringView lineQuiet{"#1A1C1F"};
constexpr QLatin1StringView lineRow{"#141618"};
constexpr QLatin1StringView textPrimary{"#F2F4F5"};
constexpr QLatin1StringView textSecondary{"#9BA1A8"};
constexpr QLatin1StringView textMuted{"#6A7178"};
constexpr QLatin1StringView textDisabled{"#4A5056"};
constexpr QLatin1StringView textPlaceholderCaption{"#3A4046"};
constexpr QLatin1StringView accent{"#5AA9D6"};
constexpr QLatin1StringView accentInk{"#07131A"};
constexpr QLatin1StringView critical{"#E0603C"};
constexpr QLatin1StringView criticalInk{"#160703"};
constexpr QLatin1StringView warning{"#E2A43C"};
constexpr QLatin1StringView positive{"#4FB286"};
constexpr QLatin1StringView positiveInk{"#04170F"};
constexpr QLatin1StringView stripeDark{"#0E0F11"};
constexpr QLatin1StringView stripeLight{"#141618"};

inline QColor rgba(int r, int g, int b, double a) { return QColor(r, g, b, static_cast<int>(a * 255.0 + 0.5)); }
inline QColor tintAccent() { return rgba(90, 169, 214, .14); }
inline QColor tintCritical() { return rgba(224, 96, 60, .14); }
inline QColor tintWarning() { return rgba(226, 164, 60, .14); }
inline QColor tintPositive() { return rgba(79, 178, 134, .14); }
inline QColor tintNeutral() { return rgba(106, 113, 120, .16); }
inline QColor tintCriticalRow() { return rgba(224, 96, 60, .06); }
inline QColor accentBadge() { return rgba(90, 169, 214, .9); }
inline QColor scrimChip() { return rgba(5, 6, 7, .72); }
inline QColor scrimBadge() { return rgba(5, 6, 7, .8); }
inline QColor scrimCaption() { return rgba(5, 6, 7, .82); }
inline QColor scrimControls() { return rgba(5, 6, 7, .85); }
inline QColor scrimBackdrop() { return rgba(5, 6, 7, .86); }
inline QColor scrimFooterStart() { return rgba(5, 6, 7, 0); }
inline QColor scrimFooterEnd() { return rgba(5, 6, 7, .9); }
inline QColor shadow() { return rgba(0, 0, 0, .6); }

inline QColor q(QLatin1StringView hex) { return QColor(QString(hex)); }
}

// Fixed geometry (handoff-map section 3, README "Spacing, radius, geometry").
namespace size {
constexpr int titleBar = 40;
constexpr int tabBar = 52;
constexpr int subTabBar = 44;
constexpr int toolbar = 48;
constexpr int panelHeader = 48;
constexpr int dialogHeader = 52;
constexpr int dialogFooter = 60;
constexpr int tableHeader = 34;
constexpr int treeRow = 30;
constexpr int treeRowGap = 2;
constexpr int alertRow = 76;
constexpr int railFooter = 44;

constexpr int cameraRail = 248;
constexpr int rulesRail = 400;
constexpr int conceptsRail = 320;
constexpr int inspector = 340;
constexpr int conceptStatus = 360;
constexpr int askColumn = 420;
constexpr int dialog = 880;
constexpr int dialogTabList = 180;
constexpr int layoutPopup = 236;

constexpr int windowMinWidth = 1440;
constexpr int windowMinHeight = 900;
constexpr int windowDesignWidth = 1920;
constexpr int windowDesignHeight = 1080;

constexpr int titleBarPaddingX = 14;
constexpr int titleBrandGap = 10;
constexpr int titleButtonGap = 18;
constexpr int brandMark = 16;
constexpr int tabBarPaddingX = 20;
constexpr int tabPaddingX = 16;
constexpr int tabGap = 4;
constexpr int tabIndicator = 2;
constexpr int tabBadgeHeight = 17;

constexpr int railHeaderPaddingX = 16;
constexpr int railHeaderGap = 6;
constexpr int railFilterTop = 12;
constexpr int railFilterSide = 12;
constexpr int railFilterHeight = 30;
constexpr int treePaddingY = 12;
constexpr int treePaddingX = 8;
constexpr int treeRowPaddingX = 8;
constexpr int treeGlyphGap = 8;
constexpr int treeDot = 6;
constexpr int glyphTree = 11;
constexpr int glyphCaret = 8;
constexpr int glyphTitleBar = 12;
constexpr int glyphRail = 12;
constexpr int glyphDialogClose = 13;

constexpr int wallToolbarPaddingX = 20;
constexpr int wallToolbarGap = 8;
constexpr int wallPadding = 12;
constexpr int wallGap = 10;
constexpr int tileChipInset = 8;
constexpr int tileChipGap = 6;
constexpr int tileChipHeight = 20;
constexpr int tileChipPaddingX = 7;
constexpr int tileStateDot = 5;
constexpr int tileStateGap = 5;
constexpr int tileFooter = 34;
constexpr int tileFooterPaddingX = 10;
constexpr int tileFooterPaddingBottom = 8;

constexpr int popupOffset = 6;
constexpr int popupPadding = 6;
constexpr int popupGap = 2;
constexpr int popupRow = 32;
constexpr int popupRowPaddingX = 12;

constexpr int buttonXs = 24;
constexpr int buttonSm = 28;
constexpr int buttonMd = 30;
constexpr int buttonLg = 32;
constexpr int input = 34;
constexpr int inputSm = 30;
constexpr int avatar = 32;
constexpr int toggleMdWidth = 34;
constexpr int toggleMdHeight = 18;
constexpr int toggleSmWidth = 30;
constexpr int toggleSmHeight = 16;
}

namespace radius {
constexpr int pill = 1;
constexpr int box = 2;
constexpr int chip = 4;
constexpr int control = 5;
constexpr int input = 6;
constexpr int listCard = 7;
constexpr int card = 8;
constexpr int toggle = 9;
constexpr int dialog = 12;
constexpr int avatar = 16;
}

namespace font {
constexpr QLatin1StringView sans{"IBM Plex Sans"};
constexpr QLatin1StringView mono{"IBM Plex Mono"};
constexpr int micro = 10;
constexpr int label = 11;
constexpr int small = 12;
constexpr int body = 13;
constexpr int emphasis = 14;
constexpr int subhead = 15;
constexpr int query = 17;
constexpr int heading = 20;
constexpr int displaySmall = 28;
constexpr int display = 30;
constexpr double labelSpacingPx = 1.32;
constexpr double captionSpacingPx = 1.1;
constexpr double captionMicroSpacingPx = 1.0;
constexpr double displaySpacingPx = -0.6;
constexpr double brandSpacingPx = 0.13;
}

namespace stripe {
constexpr double angleDeg = 115.0;
constexpr int lightBand = 3;
constexpr int period = 9;
constexpr double wallOpacity = .85;
}

namespace motion {
constexpr int livePulseMs = 2000;
constexpr double livePulseMin = .35;
constexpr int popupFadeMs = 140;
}

}

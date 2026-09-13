#pragma once
#include <QString>

namespace fovea::ui {

enum class WallLayout { OneByOne = 1, TwoByTwo = 2, ThreeByThree = 3 };

inline int wallColumns(WallLayout layout) { return static_cast<int>(layout); }
inline int wallSlots(WallLayout layout) { return wallColumns(layout) * wallColumns(layout); }
inline QString wallLayoutLabel(WallLayout layout) {
  const int n = wallColumns(layout);
  return QStringLiteral("%1×%1").arg(n);
}
inline WallLayout wallLayoutFromColumns(int columns, WallLayout fallback) {
  switch (columns) {
    case 1: return WallLayout::OneByOne;
    case 2: return WallLayout::TwoByTwo;
    case 3: return WallLayout::ThreeByThree;
    default: return fallback;
  }
}

}

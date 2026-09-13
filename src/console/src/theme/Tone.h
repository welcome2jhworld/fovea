#pragma once
#include "theme/Tokens.h"
#include <QColor>

namespace fovea::ui {

// Status tones shared by chips, severity bars and delegates.
enum class Tone { Neutral, Info, Positive, Warning, Critical };

inline QColor toneColor(Tone tone) {
  switch (tone) {
    case Tone::Info: return tokens::color::q(tokens::color::accent);
    case Tone::Positive: return tokens::color::q(tokens::color::positive);
    case Tone::Warning: return tokens::color::q(tokens::color::warning);
    case Tone::Critical: return tokens::color::q(tokens::color::critical);
    case Tone::Neutral: break;
  }
  return tokens::color::q(tokens::color::textMuted);
}

inline QColor toneTint(Tone tone) {
  switch (tone) {
    case Tone::Info: return tokens::color::tintAccent();
    case Tone::Positive: return tokens::color::tintPositive();
    case Tone::Warning: return tokens::color::tintWarning();
    case Tone::Critical: return tokens::color::tintCritical();
    case Tone::Neutral: break;
  }
  return tokens::color::tintNeutral();
}

}

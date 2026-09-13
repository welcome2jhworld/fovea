#pragma once
#include <QString>
#include <cstdint>

namespace fovea::ui {

// GStreamer caps or codec names to the display label (video/x-h265 -> H.265).
QString codecLabel(const QString& codec);
// "mm:ss"; hours fold into the minutes field.
QString clockLabel(int64_t ms);
// Seconds with one decimal below a minute, otherwise "m min s".
QString durationLabel(int64_t ms);
// Local wall-clock time; the date is prefixed when it is not today.
QString localTimeLabel(int64_t utcMs);
QString bitrateLabel(double kbps);

}

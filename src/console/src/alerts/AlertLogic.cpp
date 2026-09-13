#include "alerts/AlertLogic.h"
#include "fovea/FrameRing.h"
#include <QStringList>
#include <algorithm>
#include <cstdlib>

namespace fovea::ui {

namespace {
constexpr int kMinutesPerDay = 24 * 60;
constexpr int kAllDays = 0x7f;
constexpr int64_t kNsPerSecond = 1'000'000'000;
const char* const kDayNames[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

// 00:00-00:00 and 00:00-24:00. Any other window with end <= start wraps past midnight.
bool allDay(const ScheduleWindow& w) { return w.startMinute == 0 && (w.endMinute == 0 || w.endMinute >= kMinutesPerDay); }

bool scheduleIsAlways(const QVector<ScheduleWindow>& schedule) {
  return std::any_of(schedule.begin(), schedule.end(),
                     [](const ScheduleWindow& w) { return (w.days & kAllDays) == kAllDays && allDay(w); });
}

QString windowLabel(const ScheduleWindow& w) {
  const QString days = daysLabel(w.days);
  if (allDay(w)) return w.days == kAllDays ? QStringLiteral("all day") : QStringLiteral("%1, all day").arg(days);
  const QString next = w.endMinute <= w.startMinute ? QStringLiteral(" next day") : QString();
  return QStringLiteral("%1 %2–%3%4").arg(days, minuteLabel(w.startMinute), minuteLabel(w.endMinute), next);
}

QString secondsLabel(int64_t ns) {
  const double seconds = static_cast<double>(ns) / static_cast<double>(kNsPerSecond);
  return QStringLiteral("%1 s").arg(QString::number(seconds, 'f', ns % kNsPerSecond == 0 ? 0 : 1));
}
}

AlertBucket alertBucket(const EventInfo& event) {
  if (event.operatorState == QLatin1StringView("resolved")) return AlertBucket::Dismissed;
  if (event.review && event.review->label == QLatin1StringView("false_alarm")) return AlertBucket::Dismissed;
  if (event.operatorState == QLatin1StringView("acknowledged")) return AlertBucket::Acknowledged;
  return AlertBucket::Unresolved;
}

Tone severityTone(const QString& severity) {
  if (severity == QLatin1StringView("critical")) return Tone::Critical;
  if (severity == QLatin1StringView("review")) return Tone::Warning;
  if (severity == QLatin1StringView("info")) return Tone::Info;
  return Tone::Neutral;
}

QString severityLabel(const QString& severity) { return severity.isEmpty() ? QStringLiteral("—") : severity.toUpper(); }

Tone evidenceTone(const QString& state) {
  if (state == QLatin1StringView("available")) return Tone::Positive;
  if (state == QLatin1StringView("partial")) return Tone::Warning;
  if (state == QLatin1StringView("pending")) return Tone::Info;
  return Tone::Neutral;
}

QString operatorStateLabel(const QString& state) {
  if (state == QLatin1StringView("new")) return QStringLiteral("Unresolved");
  if (state == QLatin1StringView("acknowledged")) return QStringLiteral("Acknowledged");
  if (state == QLatin1StringView("resolved")) return QStringLiteral("Resolved");
  return state;
}

QString reviewLabel(const QString& label) {
  if (label == QLatin1StringView("confirmed")) return QStringLiteral("Confirmed");
  if (label == QLatin1StringView("false_alarm")) return QStringLiteral("False alarm");
  if (label == QLatin1StringView("undecided")) return QStringLiteral("Undecided");
  return label;
}

QString minuteLabel(int minute) {
  const int m = std::clamp(minute, 0, kMinutesPerDay);
  return QStringLiteral("%1:%2").arg(m / 60, 2, 10, QLatin1Char('0')).arg(m % 60, 2, 10, QLatin1Char('0'));
}

QString daysLabel(int days) {
  const int mask = days & kAllDays;
  if (mask == kAllDays) return QStringLiteral("every day");
  if (mask == 0) return QStringLiteral("no days");
  QStringList parts;
  int day = 0;
  while (day < 7) {
    if (!(mask & (1 << day))) {
      ++day;
      continue;
    }
    int end = day;
    while (end + 1 < 7 && (mask & (1 << (end + 1)))) ++end;
    const int run = end - day + 1;
    if (run >= 3) {
      parts << QStringLiteral("%1–%2").arg(QLatin1StringView(kDayNames[day]), QLatin1StringView(kDayNames[end]));
    } else {
      for (int d = day; d <= end; ++d) parts << QString::fromLatin1(kDayNames[d]);
    }
    day = end + 1;
  }
  return parts.join(QStringLiteral(", "));
}

QString scheduleLabel(const QVector<ScheduleWindow>& schedule) {
  if (schedule.isEmpty() || scheduleIsAlways(schedule)) return QStringLiteral("at any time");
  QStringList parts;
  for (const ScheduleWindow& w : schedule) parts << windowLabel(w);
  return parts.join(QStringLiteral(" and "));
}

QString dwellLabel(int64_t ns) {
  const int64_t seconds = ns / kNsPerSecond;
  if (seconds < 60 || ns % kNsPerSecond != 0) return secondsLabel(ns);
  if (seconds % 60 == 0) return QStringLiteral("%1 min").arg(seconds / 60);
  return QStringLiteral("%1 min %2 s").arg(seconds / 60).arg(seconds % 60);
}

QString triggerSentence(const RuleInfo& rule, const QString& zoneName, const QString& cameraLabel) {
  const QString zone = zoneName.isEmpty() ? QStringLiteral("the zone") : QStringLiteral("“%1”").arg(zoneName);
  const QString camera = cameraLabel.isEmpty() ? QStringLiteral("an unknown camera") : cameraLabel;
  QString when = scheduleLabel(rule.schedule);
  if (!scheduleIsAlways(rule.schedule) && !rule.timeZone.isEmpty()) when += QStringLiteral(" (%1)").arg(rule.timeZone);
  const int confidence = static_cast<int>(rule.minConfidence * 100.0 + 0.5);
  return QStringLiteral("Alert when a %1 stays in %2 on %3 for %4, %5, confidence ≥ %6%.")
      .arg(rule.targetClass, zone, camera, dwellLabel(rule.dwellNs), when, QString::number(confidence));
}

bool detectionMatchesFrame(const QString& detectionSessionId, int64_t detectionPtsNs,
                           const std::array<uint8_t, 16>& frameSession, int64_t framePtsNs) {
  static constexpr std::array<uint8_t, 16> kNoSession{};
  if (detectionSessionId.isEmpty() || frameSession == kNoSession) return false;
  if (fovea::sessionIdBytes(detectionSessionId) != frameSession) return false;
  return std::llabs(framePtsNs - detectionPtsNs) <= kOverlayMaxAgeNs;
}

QVector<ActivityLine> activityLines(const EventInfo& event) {
  QVector<ActivityLine> lines;
  bool sawTrigger = false;
  bool sawClear = false;
  for (const EvaluationInfo& e : event.evaluations) {
    sawTrigger = sawTrigger || e.transition == QLatin1StringView("triggered");
    sawClear = sawClear || e.transition == QLatin1StringView("cleared");
    QStringList parts{QString(e.transition).replace(QLatin1Char('_'), QLatin1Char(' '))};
    if (e.dwellNs > 0) parts << QStringLiteral("dwell %1").arg(secondsLabel(e.dwellNs));
    if (!e.trackIds.isEmpty())
      parts << QStringLiteral("%1 %2").arg(e.trackIds.size() == 1 ? QStringLiteral("track") : QStringLiteral("tracks"),
                                           e.trackIds.join(QStringLiteral(", ")));
    if (e.quality == QLatin1StringView("unknown")) parts << QStringLiteral("quality unknown");
    if (!e.note.isEmpty()) parts << e.note;
    lines.push_back({e.utcMs, parts.join(QStringLiteral(" · "))});
  }
  if (!sawTrigger && event.openedUtcMs > 0) lines.push_back({event.openedUtcMs, QStringLiteral("event opened")});
  if (!sawClear && event.clearedUtcMs > 0) lines.push_back({event.clearedUtcMs, QStringLiteral("condition cleared")});
  for (const DeliveryInfo& d : event.deliveries) {
    const bool sound = d.channel == QLatin1StringView("sound");
    if (d.state == QLatin1StringView("delivered")) {
      lines.push_back({d.deliveredUtcMs, sound ? QStringLiteral("sound played") : QStringLiteral("shown on console")});
    } else if (d.state == QLatin1StringView("failed")) {
      QString text = QStringLiteral("%1 delivery failed after %2 attempts").arg(d.channel).arg(d.attempts);
      if (!d.lastError.isEmpty()) text += QStringLiteral(" · %1").arg(d.lastError);
      lines.push_back({d.createdUtcMs, text});
    }
  }
  if (event.review) {
    QString text = QStringLiteral("reviewed: %1").arg(reviewLabel(event.review->label).toLower());
    if (!event.review->note.isEmpty()) text += QStringLiteral(" · %1").arg(event.review->note);
    lines.push_back({event.review->utcMs, text});
  }
  std::stable_sort(lines.begin(), lines.end(), [](const ActivityLine& a, const ActivityLine& b) { return a.utcMs > b.utcMs; });
  return lines;
}

}

#pragma once
#include "core/AlertTypes.h"
#include "theme/Tone.h"
#include <QString>
#include <QVector>
#include <array>
#include <cstdint>

// Display rules for alerts, rules and overlays. No widgets and no I/O, so the
// wording and the freshness rule are unit tested.
namespace fovea::ui {

enum class AlertBucket { Unresolved, Acknowledged, Dismissed };

// The design's Dismissed tab holds resolved events and events reviewed as false alarms.
AlertBucket alertBucket(const EventInfo& event);

Tone severityTone(const QString& severity);
QString severityLabel(const QString& severity);
Tone evidenceTone(const QString& state);
QString operatorStateLabel(const QString& state);
QString reviewLabel(const QString& label);

// "07:30"; minute 1440 prints as "24:00".
QString minuteLabel(int minute);
// Bit 0 = Monday: 0x7f "every day", 0x1f "Mon–Fri", 0x15 "Mon, Wed, Fri".
QString daysLabel(int days);
// A window ending at or before its start runs into the next day ("08:00–08:00 next day").
QString scheduleLabel(const QVector<ScheduleWindow>& schedule);
QString dwellLabel(int64_t ns);
QString triggerSentence(const RuleInfo& rule, const QString& zoneName, const QString& cameraLabel);

inline constexpr int64_t kOverlayMaxAgeNs = 1'000'000'000;
// Boxes belong on a displayed frame of the same session whose media time is within kOverlayMaxAgeNs.
bool detectionMatchesFrame(const QString& detectionSessionId, int64_t detectionPtsNs,
                           const std::array<uint8_t, 16>& frameSession, int64_t framePtsNs);

struct ActivityLine {
  int64_t utcMs = 0;
  QString text;
};
// Evaluations, deliveries and the review of one event, newest first.
QVector<ActivityLine> activityLines(const EventInfo& event);

}

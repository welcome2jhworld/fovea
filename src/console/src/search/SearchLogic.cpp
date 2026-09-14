#include "search/SearchLogic.h"
#include <QDateTime>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace fovea::ui {

TimeRange rangeFor(RangePreset preset, int64_t nowUtcMs, const TimeRange& custom) {
  switch (preset) {
    case RangePreset::LastHour: return {nowUtcMs - kHourMs, nowUtcMs};
    case RangePreset::LastDay: return {nowUtcMs - kDayMs, nowUtcMs};
    case RangePreset::LastWeek: return {nowUtcMs - kWeekMs, nowUtcMs};
    case RangePreset::Custom: break;
  }
  return custom;
}

QString rangeLabel(RangePreset preset, const TimeRange& custom) {
  switch (preset) {
    case RangePreset::LastHour: return QStringLiteral("Last 1 hour");
    case RangePreset::LastDay: return QStringLiteral("Last 24 hours");
    case RangePreset::LastWeek: return QStringLiteral("Last 7 days");
    case RangePreset::Custom: break;
  }
  const QString format = QStringLiteral("MM-dd HH:mm");
  return QStringLiteral("%1 – %2").arg(QDateTime::fromMSecsSinceEpoch(custom.fromUtcMs).toLocalTime().toString(format),
                                       QDateTime::fromMSecsSinceEpoch(custom.toUtcMs).toLocalTime().toString(format));
}

QString cameraFilterLabel(int selected, int total, const QString& singleName) {
  if (selected <= 0 || selected >= total) return QStringLiteral("All cameras");
  if (selected == 1 && !singleName.isEmpty()) return singleName;
  return QStringLiteral("%1 cameras").arg(selected);
}

QString relevanceLabel(double relevance) {
  return QStringLiteral("relevance %1").arg(QString::number(relevance, 'f', 2));
}

int coveragePercent(double ratio) {
  const double clamped = std::clamp(ratio, 0.0, 1.0);
  if (clamped >= 1.0) return 100;
  return std::min(99, static_cast<int>(std::floor(clamped * 100.0)));
}

bool coverageIncomplete(const SearchStatsInfo& stats) { return stats.coverageRatio && *stats.coverageRatio < 1.0; }

QString hoursLabel(double hours) {
  const double clamped = std::max(0.0, hours);
  const double rounded = std::round(clamped * 10.0) / 10.0;
  if (clamped > 0.0 && rounded == 0.0) return QStringLiteral("< 0.1 hours");
  const QString number = QString::number(rounded, 'f', rounded == std::floor(rounded) ? 0 : 1);
  return QStringLiteral("%1 %2").arg(number, rounded == 1.0 ? QStringLiteral("hour") : QStringLiteral("hours"));
}

QString elapsedLabel(int64_t ms) {
  const int64_t clamped = std::max<int64_t>(0, ms);
  return QStringLiteral("%1 s").arg(QString::number(static_cast<double>(clamped) / 1000.0, 'f', clamped < 1000 ? 2 : 1));
}

QString statsLine(int resultCount, const SearchStatsInfo& stats) {
  QStringList parts;
  parts << (resultCount == 1 ? QStringLiteral("1 result") : QStringLiteral("%1 results").arg(resultCount));
  parts << QStringLiteral("%1 searched").arg(hoursLabel(stats.hoursScanned));
  if (stats.coverageRatio) parts << QStringLiteral("coverage %1%").arg(coveragePercent(*stats.coverageRatio));
  parts << elapsedLabel(stats.totalMs);
  return parts.join(QStringLiteral(" · "));
}

QString coverageExplanation(const SearchStatsInfo& stats) {
  if (!coverageIncomplete(stats)) return {};
  const int missing = 100 - coveragePercent(*stats.coverageRatio);
  return QStringLiteral("About %1% of the recorded footage in this range is not indexed yet or belongs to cameras "
                        "with indexing off. That footage was not searched, so moments in it cannot appear here.")
      .arg(missing);
}

QString resultLengthLabel(const SearchResultInfo& result) {
  const int64_t seconds = (result.endUtcMs - result.startUtcMs) / 1000;
  if (seconds <= 0) return QStringLiteral("1 sample");
  return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

EmptyAction emptyActionFor(RangePreset preset, bool camerasFiltered) {
  if (preset == RangePreset::LastHour || preset == RangePreset::LastDay) return EmptyAction::WidenRange;
  if (camerasFiltered) return EmptyAction::AllCameras;
  return EmptyAction::EditQuery;
}

QString emptyActionLabel(EmptyAction action) {
  switch (action) {
    case EmptyAction::WidenRange: return QStringLiteral("Search the last 7 days");
    case EmptyAction::AllCameras: return QStringLiteral("Search all cameras");
    case EmptyAction::EditQuery: break;
  }
  return QStringLiteral("Edit the query");
}

QString emptySentence(const SearchStatsInfo& stats) {
  if (stats.samplesScanned <= 0) return QStringLiteral("No indexed footage exists in this range, so nothing was searched.");
  return QStringLiteral("The indexed footage in this range returned no results.");
}

QString indexVersionLabel(const SearchResponseInfo& response) {
  if (response.indexVersionName.isEmpty() || response.indexVersionName == response.indexVersion) return response.indexVersion;
  if (response.indexVersion.isEmpty()) return response.indexVersionName;
  return QStringLiteral("%1 · %2").arg(response.indexVersionName, response.indexVersion);
}

QString indexStatusLine(const IndexStatusInfo& status, const QHash<QString, QString>& names) {
  QStringList parts;
  parts << (status.indexVersion.isEmpty() ? QStringLiteral("Index") : QStringLiteral("Index %1").arg(status.indexVersion));
  if (status.state == QLatin1StringView("waiting_worker")) parts << QStringLiteral("waiting for the model worker");
  else if (status.state == QLatin1StringView("disabled")) parts << QStringLiteral("no model worker, indexing stopped");
  else if (status.state == QLatin1StringView("paused")) parts << QStringLiteral("paused");
  for (const IndexCameraInfo& camera : status.cameras) {
    const QString name = names.value(camera.cameraId, camera.cameraId.left(8));
    if (!camera.indexEnabled) parts << QStringLiteral("%1 off").arg(name);
    else if (camera.coverageRatio) parts << QStringLiteral("%1 %2%").arg(name).arg(coveragePercent(*camera.coverageRatio));
    else parts << QStringLiteral("%1 —").arg(name);
  }
  if (status.queued == 0 && status.running == 0) parts << QStringLiteral("queue empty");
  if (status.queued > 0) parts << QStringLiteral("%1 queued").arg(status.queued);
  if (status.running > 0) parts << QStringLiteral("%1 running").arg(status.running);
  if (status.failed > 0) parts << QStringLiteral("%1 failed").arg(status.failed);
  return parts.join(QStringLiteral(" · "));
}

}

#pragma once
#include "core/SearchTypes.h"
#include <QHash>
#include <QString>
#include <cstdint>

// Display rules for search results, filters and index status. No widgets and no I/O, so
// the wording (relevance is never a percentage or a match) is unit tested.
namespace fovea::ui {

enum class RangePreset { LastHour, LastDay, LastWeek, Custom };

struct TimeRange {
  int64_t fromUtcMs = 0;
  int64_t toUtcMs = 0;
};

inline constexpr int64_t kHourMs = 3'600'000;
inline constexpr int64_t kDayMs = 24 * kHourMs;
inline constexpr int64_t kWeekMs = 7 * kDayMs;

// Presets end at nowUtcMs; Custom returns the stored range unchanged.
TimeRange rangeFor(RangePreset preset, int64_t nowUtcMs, const TimeRange& custom);
QString rangeLabel(RangePreset preset, const TimeRange& custom);
// "All cameras" when nothing or everything is selected, the camera's name for one, "N cameras" otherwise.
QString cameraFilterLabel(int selected, int total, const QString& singleName);

// "relevance 0.31": a ranking score, two decimals, no percent sign and no "match".
QString relevanceLabel(double relevance);
// Whole percent rounded down, so coverage short of 1.0 never reads 100 %.
int coveragePercent(double ratio);
bool coverageIncomplete(const SearchStatsInfo& stats);
// "8 results · 24 hours searched · coverage 87% · 0.74 s"; coverage is left out when not reported.
QString statsLine(int resultCount, const SearchStatsInfo& stats);
QString hoursLabel(double hours);
QString elapsedLabel(int64_t ms);
// Why coverage is below 100 %, for the stats tooltip; empty when coverage is complete or unknown.
QString coverageExplanation(const SearchStatsInfo& stats);
// Card badge: "0:08"; a range without length reads "1 sample".
QString resultLengthLabel(const SearchResultInfo& result);

// The one action offered with an empty result: widen the time range, then the camera filter, then edit the query.
enum class EmptyAction { WidenRange, AllCameras, EditQuery };
EmptyAction emptyActionFor(RangePreset preset, bool camerasFiltered);
QString emptyActionLabel(EmptyAction action);
QString emptySentence(const SearchStatsInfo& stats);

// "siglip2-b16-224 · 3f9a1c0d2b7e": readable name and hash of the version that answered.
QString indexVersionLabel(const SearchResponseInfo& response);
// "Index siglip2-b16-224 · North Gate 100% · Terminal Lobby 62% · 3 queued · 1 running"; names maps camera ids.
// A scheduler that cannot index (no worker, paused) says so after the version.
QString indexStatusLine(const IndexStatusInfo& status, const QHash<QString, QString>& names);

}

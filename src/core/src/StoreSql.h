#pragma once
#include <QString>
#include <QStringList>
#include <QVariant>
#include <cstdint>

namespace fovea::core::sql {

// SQLite columns are NOT NULL; a null QString would bind as NULL.
inline QVariant text(const QString& s) { return s.isNull() ? QVariant(QStringLiteral("")) : QVariant(s); }
inline QVariant integer(int64_t v) { return QVariant(static_cast<qlonglong>(v)); }

// Schema version 3: zones, rules, evaluations, events, reviews, deliveries,
// evidence refs and analysis coverage.
QStringList analyticsSchemaStatements();

// Schema version 4: index versions and jobs, embedding records, search
// sessions, imports and the cameras' index_enabled column.
QStringList indexSchemaStatements();

}

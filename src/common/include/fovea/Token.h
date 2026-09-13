#pragma once
#include <QString>

namespace fovea {
QString loadOrCreateToken(const QString& path);
QString loadToken(const QString& path);
bool tokenEquals(const QString& a, const QString& b);
}

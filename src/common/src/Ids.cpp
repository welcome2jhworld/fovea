#include "fovea/Ids.h"
#include <QUuid>

namespace fovea {
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool isValidId(const QString& id) { return !QUuid::fromString(id).isNull(); }
}

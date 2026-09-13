#pragma once
#include <QtGlobal>

namespace fovea::ui {

bool processAlive(qint64 pid);
// The pid written to <data>/core.json by the service that last bound its port; 0 when absent.
qint64 recordedCorePid();

}

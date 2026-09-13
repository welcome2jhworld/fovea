#pragma once
#include <QString>

namespace fovea::ui {

struct StreamSource {
  QString kind;
  QString url;
  QString username;
  QString password;
  QString transport;
};

// ffplay invocation equivalent to the connection form; credentials are
// embedded in the URL, so the caller must say so when copying it.
QString ffplayCommand(const StreamSource& source);
QString shellQuote(const QString& value);

}

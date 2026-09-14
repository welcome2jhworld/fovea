#pragma once
#include "screens/search/ResultGrid.h"
#include <QVector>
#include <QWidget>
#include <optional>

class QLabel;
class QStackedWidget;

namespace fovea::ui {

class CoreClient;
class EvidencePlayer;
class ThumbnailCache;

// Inspector (340): evidence player limited to the result's range, camera and time,
// metadata, the similarity note and the actions that are not implemented yet.
class SearchInspector : public QWidget {
  Q_OBJECT
public:
  SearchInspector(CoreClient& client, ThumbnailCache& thumbnails, QWidget* parent = nullptr);
  // indexVersion (name and hash) and model describe the query that produced the row.
  void setResult(const std::optional<SearchResultRow>& row, const QString& indexVersion, const QString& model);
  void togglePlayback();

private:
  void setMeta(int row, const QString& value);

  EvidencePlayer* player_ = nullptr;
  QStackedWidget* pages_ = nullptr;
  QLabel* relevance_ = nullptr;
  QLabel* title_ = nullptr;
  QLabel* range_ = nullptr;
  QVector<QLabel*> metaValues_;
};

}

#pragma once
#include "core/SearchTypes.h"
#include "fovea/Api.h"
#include "search/SearchLogic.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <optional>

class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QStackedWidget;

namespace fovea::ui {

class CoreClient;
class CustomRangePopup;
class ResultGrid;
class SearchInspector;
class SearchResultsModel;
class SelectButton;
class ThumbnailCache;

// Screen 3: natural-language query over indexed recordings (docs/M4_DESIGN.md "Console: Search tab").
// A new query aborts the one still running; the skeleton gives way to results, the empty
// state (one sentence, one action) or the error state (reason, retry).
class SearchScreen : public QWidget {
  Q_OBJECT
public:
  enum class State { Idle, Searching, Results, Empty, Error };

  static constexpr int kResultLimit = 48;
  static constexpr int kMinGapMs = 3000;
  static constexpr int kSkeletonCards = 8;
  static constexpr int kIndexRefreshMs = 5000;

  SearchScreen(CoreClient& client, ThumbnailCache& thumbnails, QWidget* parent = nullptr);
  ~SearchScreen() override;

  void setCameras(const QVector<fovea::Camera>& cameras);
  // Focuses the query field with its text selected (Cmd/Ctrl+K from any screen).
  void focusQuery();
  // Puts the text in the query field and runs it.
  void search(const QString& query);
  void setRangePreset(RangePreset preset);
  // An empty set searches every camera.
  void setCameraFilter(const QSet<QString>& cameraIds);

  State state() const { return state_; }
  const SearchResponseInfo& response() const { return response_; }

signals:
  void stateChanged(fovea::ui::SearchScreen::State state);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  // Drops the reply and the thumbnail fetches of the results being replaced.
  void cancelSearch();
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

private:
  enum Page { GridPage, MessagePage };

  QWidget* buildQueryBlock();
  QWidget* buildResultsArea(ThumbnailCache& thumbnails);
  void submit();
  void runSearch();
  void onSearchReply(int generation, bool ok, const QJsonDocument& doc, const QString& error);
  void setState(State state);
  void showMessage(const QString& sentence, const QString& detail, bool detailIsError, const QString& action);
  void runMessageAction();
  void setStats(const QString& text, const SearchStatsInfo* stats);
  void refreshIndex();
  void renderIndex();
  void elideIndexLine();
  void showRangeMenu();
  void showCameraMenu();
  void updateFilterLabels();
  void filtersChanged();
  QJsonArray requestCameraIds() const;
  QString cameraName(const QString& id) const;
  QString cameraCode(const QString& id) const;

  CoreClient& client_;
  QVector<fovea::Camera> cameras_;
  QSet<QString> selectedCameras_;
  QSet<QString> selectionAtMenuOpen_;
  RangePreset preset_ = RangePreset::LastDay;
  TimeRange customRange_;

  QWidget* queryBar_ = nullptr;
  QLineEdit* query_ = nullptr;
  QPushButton* searchButton_ = nullptr;
  SelectButton* rangeChip_ = nullptr;
  SelectButton* cameraChip_ = nullptr;
  QMenu* rangeMenu_ = nullptr;
  QMenu* cameraMenu_ = nullptr;
  CustomRangePopup* rangePopup_ = nullptr;
  QLabel* stats_ = nullptr;
  QLabel* indexLine_ = nullptr;
  QStackedWidget* body_ = nullptr;
  ResultGrid* grid_ = nullptr;
  SearchResultsModel* model_ = nullptr;
  QLabel* messageSentence_ = nullptr;
  QLabel* messageDetail_ = nullptr;
  QPushButton* messageAction_ = nullptr;
  SearchInspector* inspector_ = nullptr;

  State state_ = State::Idle;
  QString lastQuery_;
  EmptyAction emptyAction_ = EmptyAction::EditQuery;
  SearchResponseInfo response_;
  // The cache is a member of the main window and outlives this screen only
  // until the window is destroyed, so it is held as a guarded pointer.
  QPointer<ThumbnailCache> thumbnails_;
  QPointer<QNetworkReply> activeSearch_;
  int generation_ = 0;
  QTimer indexTimer_;
  bool indexInFlight_ = false;
  std::optional<IndexStatusInfo> indexStatus_;
  QString indexError_;
  QString indexText_;
};

}

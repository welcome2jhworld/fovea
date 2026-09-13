#include "screens/monitor/CameraRail.h"
#include "screens/monitor/CameraTreeDelegate.h"
#include "screens/monitor/CameraTreeModel.h"
#include "theme/Icons.h"
#include "theme/Theme.h"
#include "theme/Tokens.h"
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace fovea::ui {
namespace tk = tokens;

CameraTree::CameraTree(QWidget* parent) : QTreeView(parent) {
  setObjectName(QStringLiteral("CameraTree"));
  setViewportMargins(tk::size::treePaddingX, tk::size::treePaddingY, tk::size::treePaddingX, tk::size::treePaddingY);
  setHeaderHidden(true);
  setRootIsDecorated(false);
  setIndentation(0);
  setFrameShape(QFrame::NoFrame);
  setUniformRowHeights(true);
  setExpandsOnDoubleClick(false);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setSelectionBehavior(QAbstractItemView::SelectRows);
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setFocusPolicy(Qt::NoFocus);
  setItemDelegate(new CameraTreeDelegate(this));
  viewport()->setAutoFillBackground(false);
}

CameraFilterProxy::CameraFilterProxy(QObject* parent) : QSortFilterProxyModel(parent) {
  setRecursiveFilteringEnabled(true);
  setAutoAcceptChildRows(true);
}

void CameraFilterProxy::setNeedle(const QString& needle) {
  needle_ = needle.trimmed();
  invalidateFilter();
}

bool CameraFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
  if (needle_.isEmpty()) return true;
  const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
  return idx.data(CameraTreeModel::SearchTextRole).toString().contains(needle_, Qt::CaseInsensitive);
}

CameraRail::CameraRail(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("CameraRail"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedWidth(tk::size::cameraRail);

  auto* header = new QWidget(this);
  header->setObjectName(QStringLiteral("RailHeader"));
  header->setAttribute(Qt::WA_StyledBackground, true);
  header->setFixedHeight(tk::size::panelHeader);
  auto* title = new QLabel(QStringLiteral("CAMERAS"), header);
  title->setObjectName(QStringLiteral("RailTitle"));
  title->setProperty("role", QStringLiteral("section-label"));
  title->setFont(Theme::monoLabel());
  auto* add = new QPushButton(QStringLiteral("＋ Add"), header);
  add->setObjectName(QStringLiteral("AddCameraButton"));
  add->setProperty("role", QStringLiteral("secondary"));
  Theme::setVariant(add, "xs");
  add->setCursor(Qt::PointingHandCursor);
  add->setFocusPolicy(Qt::NoFocus);
  auto* gear = new QToolButton(header);
  gear->setObjectName(QStringLiteral("CameraSettingsButton"));
  gear->setIcon(themedIcon(Icon::Gear, tk::color::q(tk::color::textSecondary)));
  gear->setIconSize(QSize(tk::size::glyphRail, tk::size::glyphRail));
  gear->setToolTip(QStringLiteral("Camera settings"));
  gear->setCursor(Qt::PointingHandCursor);
  gear->setFocusPolicy(Qt::NoFocus);
  auto* headerRow = new QHBoxLayout(header);
  headerRow->setContentsMargins(tk::size::railHeaderPaddingX, 0, tk::size::railHeaderPaddingX, 0);
  headerRow->setSpacing(tk::size::railHeaderGap);
  headerRow->addWidget(title);
  headerRow->addStretch(1);
  headerRow->addWidget(add);
  headerRow->addWidget(gear);

  auto* filterWrap = new QWidget(this);
  filter_ = new QLineEdit(filterWrap);
  filter_->setObjectName(QStringLiteral("CameraFilter"));
  Theme::setVariant(filter_, "sm");
  filter_->setPlaceholderText(QStringLiteral("Filter cameras…"));
  filter_->setClearButtonEnabled(false);
  filter_->setFocusPolicy(Qt::ClickFocus);
  auto* filterRow = new QVBoxLayout(filterWrap);
  filterRow->setContentsMargins(tk::size::railFilterSide, tk::size::railFilterTop, tk::size::railFilterSide, 0);
  filterRow->setSpacing(0);
  filterRow->addWidget(filter_);

  model_ = new CameraTreeModel(this);
  proxy_ = new CameraFilterProxy(this);
  proxy_->setSourceModel(model_);
  tree_ = new CameraTree(this);
  tree_->setModel(proxy_);

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(header);
  column->addWidget(filterWrap);
  column->addWidget(tree_, 1);

  connect(add, &QPushButton::clicked, this, &CameraRail::addCameraRequested);
  connect(gear, &QToolButton::clicked, this, [this] { emit cameraSettingsRequested(selectedId_); });
  connect(filter_, &QLineEdit::textChanged, this, [this](const QString& text) {
    proxy_->setNeedle(text);
    restoreViewState();
  });
  connect(model_, &CameraTreeModel::structureChanged, this, [this] {
    restoreViewState();
    if (selectedId_.isEmpty()) selectFirstCamera();
  });
  connect(tree_, &QTreeView::collapsed, this, [this](const QModelIndex& idx) {
    if (!restoring_) collapsedGroups_.insert(idx.data(CameraTreeModel::GroupNameRole).toString());
  });
  connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& idx) {
    if (!restoring_) collapsedGroups_.remove(idx.data(CameraTreeModel::GroupNameRole).toString());
  });
  connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex& idx) {
    if (idx.data(CameraTreeModel::IsGroupRole).toBool()) tree_->setExpanded(idx, !tree_->isExpanded(idx));
  });
  connect(tree_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &CameraRail::onSelectionChanged);
}

void CameraRail::setSnapshot(const QVector<fovea::Camera>& cameras, const QVector<fovea::CameraStatus>& statuses) {
  model_->setSnapshot(cameras, statuses);
}

void CameraRail::restoreViewState() {
  restoring_ = true;
  for (int r = 0; r < proxy_->rowCount(); ++r) {
    const QModelIndex idx = proxy_->index(r, 0);
    if (!idx.data(CameraTreeModel::IsGroupRole).toBool()) continue;
    tree_->setExpanded(idx, !collapsedGroups_.contains(idx.data(CameraTreeModel::GroupNameRole).toString()));
  }
  if (!selectedId_.isEmpty()) {
    const QModelIndex src = model_->indexForCamera(selectedId_);
    const QModelIndex idx = proxy_->mapFromSource(src);
    if (idx.isValid()) {
      tree_->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
    } else if (!src.isValid()) {
      selectedId_.clear();
      emit selectedCameraChanged(selectedId_);
    }
  }
  restoring_ = false;
}

void CameraRail::selectFirstCamera() {
  for (int r = 0; r < proxy_->rowCount(); ++r) {
    QModelIndex idx = proxy_->index(r, 0);
    if (idx.data(CameraTreeModel::IsGroupRole).toBool()) {
      if (proxy_->rowCount(idx) == 0) continue;
      idx = proxy_->index(0, 0, idx);
    }
    tree_->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
    return;
  }
}

void CameraRail::onSelectionChanged() {
  if (restoring_) return;
  const QModelIndexList rows = tree_->selectionModel()->selectedIndexes();
  const QString id = rows.isEmpty() ? QString() : rows.first().data(CameraTreeModel::CameraIdRole).toString();
  if (id == selectedId_) return;
  selectedId_ = id;
  emit selectedCameraChanged(selectedId_);
}

}

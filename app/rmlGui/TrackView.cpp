#include "TrackView.h"
#include "TrackSortFilterProxyModel.h"
#include "TagsDelegate.h"

TrackView::TrackView(TableModel::AbstractTrackList& tracks, QWidget* parent) : QWidget(parent)
{
  setupUi(this);
  auto* model = new TableModel{tracks, this};
  auto* proxyModel = new TrackSortFilterProxyModel{this};
  proxyModel->setSourceModel(model);
  tableView->setModel(proxyModel);

  tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  tableView->resizeColumnsToContents();
  tableView->setItemDelegateForColumn(3, new TagsDelegate{});

  connect(lineEdit, &QLineEdit::textChanged, [proxyModel](const QString& text) { proxyModel->onQuickFilterChanged(text); });
}

TrackView::~TrackView() {}

#include "TrackView.h"
#include "TrackSortFilterProxyModel.h"

TrackView::TrackView(TableModel::TrackList& tracks, QWidget* parent) : QWidget(parent)
{
  setupUi(this);
  auto* model = new TableModel{tracks, this};
  auto* proxyModel = new TrackSortFilterProxyModel{this};
  proxyModel->setSourceModel(model);
  tableView->setModel(proxyModel);

  tableView->resizeColumnsToContents();
  tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);

  connect(lineEdit, &QLineEdit::textChanged, [proxyModel](const QString& text) { proxyModel->onQuickFilterChanged(text); });
}

TrackView::~TrackView() {}

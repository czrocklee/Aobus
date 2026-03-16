// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackView.h"
#include "TagsDelegate.h"
#include "TrackSortFilterProxyModel.h"

TrackView::TrackView(TableModel::AbstractTrackList& tracks, QWidget* parent) : QWidget(parent)
{
  setupUi(this);
  auto* model = new TableModel{tracks, this};
  auto* proxyModel = new TrackSortFilterProxyModel{this};
  proxyModel->setSourceModel(model);
  tableView->setModel(proxyModel);

  tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  tableView->resizeColumnsToContents();
  tableView->setItemDelegateForColumn(3, new TagsDelegate{tableView});

  connect(
    lineEdit, &QLineEdit::textChanged, [proxyModel](QString const& text) { proxyModel->onQuickFilterChanged(text); });
}

TrackView::~TrackView() {}

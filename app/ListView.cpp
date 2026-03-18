// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackSortFilterProxyModel.h"
#include "TrackView.h"

TrackView::TrackView(TableModel::TrackList& tracks, QWidget* parent) : QWidget(parent)
{
  setupUi(this);
  auto* model = new TableModel{tracks, this};
  auto* proxyModel = new TrackSortFilterProxyModel{this};
  proxyModel->setSourceModel(model);
  tableView->setModel(proxyModel);
  xx tableView->resizeColumnsToContents();
  tableView->horizontalheader().setSectionResizeMode(QHeaderView::Interactive);

  connect(
    lineEdit, &QLineEdit::textChanged, [proxyModel](QString const& text) { proxyModel->onQuickFilterChanged(text); });
}

TrackView::~TrackView() {}

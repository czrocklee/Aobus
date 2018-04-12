/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <MainWindow.H>
#include <TableModel.H>
#include <TrackSortFilterProxyModel.H>

#include <rs/ml/core/MediaLibrary.H>

MainWindow::MainWindow()
{
  setupUi(this);
  static rs::ml::core::MediaLibrary ml{"/home/rocklee/RockStudio/mylib"};

  auto model = new TableModel{ml, this};
  auto proxy = new TrackSortFilterProxyModel{this};
  proxy->setSourceModel(model);

  tableView->setModel(proxy);
 // tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
 // tableView->horizontalHeader()->setStretchLastSection(true);
 // tableView->verticalHeader()->hide();
 // tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
 // tableView->setSelectionMode(QAbstractItemView::SingleSelection);


  connect(lineEdit, SIGNAL(textChanged(const QString&)),
          model, SLOT(onQuickFilterChanged(const QString&)));
}

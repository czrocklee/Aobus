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
#include <AddTrackDialog.H>
#include <TableModel.H>
#include <TrackSortFilterProxyModel.H>

#include <rs/ml/core/TrackListZero.H>
#include <rs/ml/query/TrackFilter.H>
#include <rs/ml/query/Parser.H>

MainWindow::MainWindow()
  : _ml{"/home/rocklee/RockStudio/mylib"}
{
  setupUi(this);

  rs::ml::query::Parser parser;
  auto expr = parser.parse("%title% ~ \"Bach\"");
  static rs::ml::core::TrackListZero root{_ml, {}};//rs::ml::query::TrackFilter{std::move(expr)}};

  auto model = new TableModel{root, this};
  auto proxy = new TrackSortFilterProxyModel{_ml, this};
  proxy->setSourceModel(model);

  tableView->setModel(proxy);
 // tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
 // tableView->horizontalHeader()->setStretchLastSection(true);
 // tableView->verticalHeader()->hide();
 // tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
 // tableView->setSelectionMode(QAbstractItemView::SingleSelection);


  connect(actionFile, &QAction::triggered, [this]
  { 
    auto dialog = new AddTrackDialog{this};

    if (dialog->exec())
    {
      auto writer = _ml.writer();
      writer.create(dialog->track());
    }
  });

  connect(lineEdit, SIGNAL(textChanged(const QString&)),
          proxy, SLOT(onQuickFilterChanged(const QString&)));
}

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

#include "MainWindow.h"
#include "AddTrackDialog.h"
#include "TableModel.h"
#include "TrackSortFilterProxyModel.h"

#include <rs/ml/reactive/ItemList.h>
#include <rs/ml/query/TrackFilter.h>
#include <rs/ml/query/Parser.h>
#include <QtCore/QDebug>

using TrackList = rs::ml::reactive::ItemList<rs::ml::core::TrackT>;

MainWindow::MainWindow(const std::string& rootDir) : _ml{rootDir}
{
  setupUi(this);

  for (const auto& track : _ml.tracks().readTransaction())
  {
    _tracks.insert(rs::ml::core::TrackT::fromItem(track));
  }

  /*    ListItem(const rs::ml::core::List& list)
       : QListWidgetItem{QString::fromUtf8(list.value->name()->c_str())}, list{rs::ml::core::ListT::fromItem(list)}
     {} */
  auto* all = new ListItem{"all", listWidget};
  all->trackView = new TrackView{_tracks, stackedWidget};
  stackedWidget->addWidget(all->trackView);

  for (const auto& list : _ml.lists().readTransaction())
  {
    auto* listItem = new ListItem{QString::fromUtf8(list.value->name()->c_str()), listWidget};
    auto expr = rs::ml::query::parse(list.value->expr()->c_str());
    //auto expr = rs::ml::query::parse("#tag1");

    listItem->tracks = std::make_unique<TrackFilterList>(_tracks, rs::ml::query::TrackFilter{std::move(expr)});
    listItem->trackView = new TrackView{*listItem->tracks, stackedWidget};
    stackedWidget->addWidget(listItem->trackView);
  }

  connect(listWidget, &QListWidget::currentItemChanged, [this](auto* curr, auto*) {
    stackedWidget->setCurrentWidget(dynamic_cast<ListItem*>(curr)->trackView);
  });


  for (auto i = 0u; i < _tracks.size(); ++i) {
    qInfo() << &_tracks.at(TrackList::Index{i});
  }


 

    // rs::ml::query::Parser parser;
    // auto expr = parser.parse("@pop and #tag");
    /*  auto tracks = std::make_unique<RootTrackList>();


     auto model = new TableModel{std::move(tracks), this};
     auto proxy = new TrackSortFilterProxyModel{_ml, this};
     proxy->setSourceModel(model);

     tableView->setModel(proxy);
     tableView->resizeColumnsToContents();
     tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive); */
    // tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    // tableView->horizontalHeader()->setStretchLastSection(true);
    // tableView->verticalHeader()->hide();
    // tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // tableView->setSelectionMode(QAbstractItemView::SingleSelection);

    connect(actionFile, &QAction::triggered, [this] {
      auto dialog = new AddTrackDialog{this};

      if (dialog->exec())
      {
        // auto writer = _ml.writer();
        // writer.create(dialog->track());
      }
    });

  // connect(lineEdit, SIGNAL(textChanged(const QString&)), proxy, SLOT(onQuickFilterChanged(const QString&)));
}

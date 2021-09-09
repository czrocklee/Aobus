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
#include "NewListDialog.h"
#include "TableModel.h"
#include "TrackSortFilterProxyModel.h"

#include <rs/ml/reactive/ItemList.h>
#include <rs/ml/expr/Evaluator.h>
#include <rs/ml/expr/Parser.h>
#include <QtCore/QDebug>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtCore/QSettings>
#include <filesystem>

// using TrackList = rs::ml::reactive::ItemList<rs::ml::core::MusicLibrary::TrackId, rs::ml::fbs::TrackT>;

MainWindow::MainWindow()
{
  _ui.setupUi(this);

  connect(_ui.actionOpen, &QAction::triggered, [this] {
    if (QString dir = QFileDialog::getExistingDirectory(
          this, tr("Open Directory"), "/home", QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        !dir.isNull())
    {
      openMusicLibrary(dir.toStdString());
    }
  });

  if (auto lastSession = QSettings{"settings.ini"}.value("session.lastMusicLibaryOpened");
      !lastSession.isNull() && std::filesystem::exists({lastSession.toString().toStdString()}))
  {
    openMusicLibrary(lastSession.toString().toStdString());
  }
}

void MainWindow::openMusicLibrary(const std::string& dir)
{
  _ml = std::make_unique<MusicLibrary>(dir);

  auto txn = _ml->readTransaction();
  loadTracks(txn);
  loadLists(txn);
  QSettings settings{"settings.ini"};
  settings.setValue("session.lastMusicLibaryOpened", QString::fromStdString(dir));
}

void MainWindow::loadTracks(ReadTransaction& txn)
{
  for (auto [id, track] : _ml->tracks().reader(txn))
  {
    [[maybe_unused]] auto cstr = [](const flatbuffers::String* str) { return str == nullptr ? "nil" : str->str(); };

    // std::cout << cstr(track->meta()->album());
    rs::ml::fbs::TrackT tt;
    track->UnPackTo(&tt);
    _allTracks.insert(id, std::move(tt));
  }
}

TrackView* MainWindow::createTrackView(TableModel::AbstractTrackList& list)
{
  auto* trackView = new TrackView{list, _ui.stackedWidget};
  _ui.stackedWidget->addWidget(trackView);
  trackView->setContextMenuPolicy(Qt::CustomContextMenu);

  connect(trackView->tableView, &QAbstractItemView::clicked, [this](const QModelIndex& index) { this->onTrackClicked(index); });
  connect(trackView, &QTableView::customContextMenuRequested, [trackView, this](QPoint pos) {
    // Handle global position
    QPoint globalPos = trackView->mapToGlobal(pos);

    // Create menu and insert some actions
    QMenu myMenu;
    myMenu.addAction("Tag", [trackView, this] {
      bool isOk;
      if (QString text = QInputDialog::getText(this, tr("New Tag"), tr("Tag:"), QLineEdit::Normal, "", &isOk); isOk)
      {
        QItemSelectionModel* select = trackView->tableView->selectionModel();
        auto txn = _ml->writeTransaction();
        auto writer = _ml->tracks().writer(txn);
        for (const QModelIndex& index : select->selectedRows())
        {
          using IdTrackPair = std::pair<rs::ml::core::MusicLibrary::TrackId, rs::ml::fbs::TrackT>;
          QModelIndex sourceIndex = static_cast<QAbstractProxyModel*>(trackView->tableView->model())->mapToSource(index);
          rs::ml::core::MusicLibrary::TrackId id = static_cast<IdTrackPair*>(sourceIndex.internalPointer())->first;
          _allTracks.update(id, [id, &writer, &text](auto& track) {
            track.tags.push_back(text.toStdString());
            writer.updateT(id, track);
          });
        }
        txn.commit();
      }

      /*       if (auto* dialog = new NewListDialog{this}; dialog->exec())
            {
              auto txn = _ml->writeTransaction();
              auto writer = _ml->lists().writer(txn);
              auto [_, list] = writer.createT(dialog->list());
              addListItem(list);
              t xn.commit();
            }*/
    });
    /*     myMenu.addAction("Delete", this, SLOT(eraseItem()));
     */
    // Show context menu at handling position
    myMenu.exec(globalPos);
  });
  return trackView;
}

void MainWindow::loadLists(ReadTransaction& txn)
{
  auto* all = new ListItem{"all", _ui.listWidget};
  all->trackView = createTrackView(_allTracks);

  for (const auto [_, list] : _ml->lists().reader(txn))
  {
    addListItem(list);
  }

  connect(_ui.listWidget, &QListWidget::currentItemChanged, [this](auto* curr, auto*) {
    _ui.stackedWidget->setCurrentWidget(dynamic_cast<ListItem*>(curr)->trackView);
  });

  _ui.listWidget->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(_ui.listWidget, &QListWidget::customContextMenuRequested, [this](QPoint pos) {
    // Handle global position
    QPoint globalPos = _ui.listWidget->mapToGlobal(pos);

    // Create menu and insert some actions
    QMenu myMenu;
    myMenu.addAction("New", [this] {
      if (auto* dialog = new NewListDialog{this}; dialog->exec())
      {
        auto txn = _ml->writeTransaction();
        auto writer = _ml->lists().writer(txn);
        auto [_, list] = writer.createT(dialog->list());
        addListItem(list);
        txn.commit();
      }
    });
    myMenu.addAction("Delete", this, SLOT(eraseItem()));

    // Show context menu at handling position
    myMenu.exec(globalPos);
  });
}

void MainWindow::onTrackClicked(const QModelIndex& index)
{
  if (QVariant resourceId = index.model()->data(index, Qt::UserRole); resourceId.isValid())
  {
    auto txn = _ml->readTransaction();
    auto data = _ml->resources().reader(txn)[resourceId.toULongLong()];

    if (QPixmap pix; pix.loadFromData(static_cast<const uchar*>(data.data()), data.size(), "JPG"))
    {
      _ui.coverArtLabel->setPixmap(pix);
    }
  }
  else
  {
    Q_INIT_RESOURCE(resources);
    _ui.coverArtLabel->setPixmap(QPixmap{":/images/nocoverart.jpg"});
  }
}

void MainWindow::addListItem(const rs::ml::fbs::List* list)
{
  auto* listItem = new ListItem{QString::fromUtf8(list->name()->c_str()), _ui.listWidget};
  listItem->tracks =
    std::make_unique<TrackFilterList>(_allTracks, [expr = rs::ml::expr::parse(list->expr()->c_str())](const auto& tt) {
      return rs::ml::expr::toBool(rs::ml::expr::evaluate(expr, tt));
    });
  listItem->trackView = createTrackView(*listItem->tracks);
}
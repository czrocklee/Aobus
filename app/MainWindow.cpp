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
#include "ImportProgressDialog.h"
#include "ImportWorker.h"
#include "NewListDialog.h"
#include "PlaylistExporter.h"
#include "TableModel.h"
#include "TrackSortFilterProxyModel.h"

#include <rs/expr/Evaluator.h>
#include <rs/expr/Parser.h>
#include <rs/reactive/ItemList.h>

#include <QtCore/QDebug>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include <boost/iterator/filter_iterator.hpp>

#include <filesystem>
#include <set>

MainWindow::MainWindow()
{
  _ui.setupUi(this);

  auto const updateWindowTitle = [this](auto const& dir) { setWindowTitle(QString("RockStudio [%1]").arg(dir)); };

  connect(_ui.actionOpen, &QAction::triggered, [this, updateWindowTitle] {
    if (QString dir = QFileDialog::getExistingDirectory(
          this, tr("Open"), QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        !dir.isNull())
    {
      if (!dir.contains("data.mdb"))
      {
        importMusicLibrary(dir.toStdString());
      }
      else
      {
        openMusicLibrary(dir.toStdString());
      }

      QSettings settings{"settings.ini"};
      settings.setValue("session.lastMusicLibaryOpened", dir);
      updateWindowTitle(dir);
    }
  });

  if (auto lastSession = QSettings{"settings.ini"}.value("session.lastMusicLibaryOpened");
      !lastSession.isNull() && std::filesystem::exists({lastSession.toString().toStdString()}))
  {
    auto dir = lastSession.toString();
    openMusicLibrary(dir.toStdString());
    updateWindowTitle(dir);
  }
}

void MainWindow::openMusicLibrary(std::string const& root)
{
  _ml = std::make_unique<MusicLibrary>(root);

  auto txn = _ml->readTransaction();
  loadTracks(txn);
  loadLists(txn);
}

namespace
{
  class FileFilter
  {
  public:
    FileFilter(std::string const& rootPath, std::vector<std::string> const& extensions)
      : _rootPath{rootPath}
      , _extensions{extensions.begin(), extensions.end()}
    {
      _filter = [this](std::filesystem::path const& path) {
        return _extensions.find(path.extension().string()) != _extensions.end();
      };
    }

    auto begin() const { return boost::make_filter_iterator(_filter, Iterator{_rootPath}); }

    auto end() const { return boost::make_filter_iterator(_filter, Iterator{}); }

  private:
    using Iterator = std::filesystem::recursive_directory_iterator;

    std::string _rootPath;
    std::set<std::string> _extensions;
    std::function<bool(std::filesystem::path const&)> _filter;
  };

}
/*   constexpr auto buildString = [](auto& fbb, const auto& value) {
    return rs::tag::isNull(value) ? ::flatbuffers::Offset<::flatbuffers::String>{}
                                  : fbb.CreateString(std::get<std::string>(value));
  }; */

void MainWindow::importMusicLibrary(std::string const& root)
{
  _ml = std::make_unique<MusicLibrary>(root);

  FileFilter filter{root, {".flac", ".m4a"}};
  std::vector<std::filesystem::path> files{filter.begin(), filter.end()};

  auto* dialog = new ImportProgressDialog{static_cast<int>(files.size()), this};
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  auto* worker = new ImportWorker{*_ml, files, this};

  connect(worker, &ImportWorker::progressUpdated, dialog, &ImportProgressDialog::onNewTrack, Qt::QueuedConnection);
  connect(worker, &ImportWorker::workFinished, dialog, &ImportProgressDialog::ready, Qt::QueuedConnection);

  worker->start();

  if (dialog->exec() == QDialog::Accepted)
  {
    worker->commit();
    worker->deleteLater();
    _allTracks.clear();
    auto txn = _ml->readTransaction();
    loadTracks(txn);
  }
}

void MainWindow::loadTracks(ReadTransaction& txn)
{
  for (auto [id, track] : _ml->tracks().reader(txn))
  {
    rs::fbs::TrackT tt;
    track->UnPackTo(&tt);
    _allTracks.insert(id, std::move(tt));
  }
}

TrackView* MainWindow::createTrackView(std::string_view name, TableModel::AbstractTrackList& list)
{
  auto* trackView = new TrackView{list, _ui.stackedWidget};
  _ui.stackedWidget->addWidget(trackView);
  trackView->setContextMenuPolicy(Qt::CustomContextMenu);

  auto playlistDir = _ml->rootPath() / "playlist";

  if (!std::filesystem::exists(playlistDir))
  {
    std::filesystem::create_directories(playlistDir);
  }

  new PlaylistExporter{list, _ml->rootPath(), playlistDir / std::format("{}.m3u", name), trackView};

  connect(trackView->tableView, &QAbstractItemView::clicked, [this](QModelIndex const& index) {
    this->onTrackClicked(index);
  });

  connect(trackView, &QTableView::customContextMenuRequested, [trackView, this](QPoint pos) {
    // Handle global position
    QPoint globalPos = trackView->mapToGlobal(pos);

    // Create menu and insert some actions
    QMenu myMenu;
    myMenu.addAction("Tag", [trackView, this] {
      bool isOk;
      QString text = QInputDialog::getText(this, tr("New Tag"), tr("Tag:"), QLineEdit::Normal, "", &isOk);

      if (isOk)
      {
        QItemSelectionModel* select = trackView->tableView->selectionModel();
        auto txn = _ml->writeTransaction();
        auto writer = _ml->tracks().writer(txn);
        for (QModelIndex const& index : select->selectedRows())
        {
          using IdTrackPair = std::pair<rs::core::MusicLibrary::TrackId, rs::fbs::TrackT>;
          QModelIndex sourceIndex =
            static_cast<QAbstractProxyModel*>(trackView->tableView->model())->mapToSource(index);
          rs::core::MusicLibrary::TrackId id = static_cast<IdTrackPair*>(sourceIndex.internalPointer())->first;
          _allTracks.update(id, [id, &writer, &text](auto& track) {
            track.tags.push_back(text.toStdString());
            writer.updateT(id, track);
          });
        }

        txn.commit();
      }
    });

    // Show context menu at handling position
    myMenu.exec(globalPos);
  });
  return trackView;
}

namespace
{
  struct ListItem : public QListWidgetItem
  {
    using TrackFilterList = rs::reactive::ItemFilterList<rs::core::MusicLibrary::TrackId, rs::fbs::TrackT>;
    using QListWidgetItem::QListWidgetItem;

    rs::core::MusicLibrary::ListId id;
    rs::fbs::ListT list;
    TrackView* trackView;
    std::unique_ptr<TrackFilterList> tracks;
  };
}

void MainWindow::loadLists(ReadTransaction& txn)
{
  auto* all = new ListItem{"all", _ui.listWidget};
  all->trackView = createTrackView("all", _allTracks);

  for (auto const [id, list] : _ml->lists().reader(txn))
  {
    addListItem(id, list);
  }

  connect(_ui.listWidget, &QListWidget::currentItemChanged, [this](auto* curr, auto*) {
    _ui.stackedWidget->setCurrentWidget(dynamic_cast<ListItem*>(curr)->trackView);
  });

  _ui.listWidget->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(_ui.listWidget, &QListWidget::customContextMenuRequested, [this](QPoint pos) {
    QMenu myMenu;
    myMenu.addAction("New", [this] {
      if (auto* dialog = new NewListDialog{this}; dialog->exec())
      {
        auto txn = _ml->writeTransaction();
        auto writer = _ml->lists().writer(txn);
        auto [id, list] = writer.createT(dialog->list());

        try
        {
          addListItem(id, list);
          txn.commit();
        }
        catch (std::exception const& ex)
        {
          QMessageBox::critical(this, "Failed to create list", ex.what());
        }
      }
    });

    myMenu.addAction("Delete", this, [this] {
      auto* item = static_cast<ListItem*>(_ui.listWidget->takeItem(_ui.listWidget->currentRow()));
      auto txn = _ml->writeTransaction();
      auto writer = _ml->lists().writer(txn);
      writer.del(item->id);
      txn.commit();
    });

    QPoint globalPos = _ui.listWidget->mapToGlobal(pos);
    myMenu.exec(globalPos);
  });
}

void MainWindow::onTrackClicked(QModelIndex const& index)
{
  if (QVariant resourceId = index.model()->data(index, Qt::UserRole); resourceId.isValid())
  {
    auto txn = _ml->readTransaction();
    auto data = _ml->resources().reader(txn)[resourceId.toULongLong()];

    if (QPixmap pix; pix.loadFromData(static_cast<uchar const*>(data.data()), static_cast<uint>(data.size())))
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

void MainWindow::addListItem(rs::core::MusicLibrary::ListId id, rs::fbs::List const* list)
{
  auto expr = rs::expr::parse(list->expr()->string_view());
  auto* listItem = new ListItem{QString::fromUtf8(list->name()->c_str()), _ui.listWidget};
  listItem->id = id;
  listItem->tracks = std::make_unique<ListItem::TrackFilterList>(
    _allTracks, [expr](auto const& tt) { return rs::expr::toBool(rs::expr::evaluate(expr, tt)); });
  listItem->trackView = createTrackView(list->name()->string_view(), *listItem->tracks);
}

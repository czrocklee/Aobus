// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/MainWindow.h"
#include "core/Log.h"

#include <rs/core/ListBuilder.h>
#include <rs/core/MusicLibrary.h>
#include <rs/core/ResourceStore.h>
#include <rs/core/TrackBuilder.h>

#include "core/ImportWorker.h"
#include "core/model/AllTrackIdsList.h"
#include "core/model/FilteredTrackIdList.h"
#include "core/model/ListDraft.h"
#include "core/model/ManualTrackIdList.h"
#include "core/model/TrackIdList.h"
#include "core/model/TrackRowDataProvider.h"
#include "core/playback/PlaybackController.h"
#include "platform/linux/services/PlaylistExporter.h"
#include "platform/linux/ui/CoverArtWidget.h"
#include "platform/linux/ui/ImportProgressDialog.h"
#include "platform/linux/ui/ListRow.h"
#include "platform/linux/ui/ListTreeNode.h"
#include "platform/linux/ui/PlaybackBar.h"
#include "platform/linux/ui/SmartListDialog.h"
#include "platform/linux/ui/TagPopover.h"
#include "platform/linux/ui/TrackListAdapter.h"
#include "platform/linux/ui/TrackViewPage.h"

#include <glibmm/keyfile.h>
#include <glibmm/variant.h>
#include <gtkmm/filedialog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

namespace app::ui
{

  namespace
  {
    rs::core::ListId rootParentId()
    {
      return rs::core::ListId{0};
    }

    rs::core::ListId allTracksListId()
    {
      return rs::core::ListId{std::numeric_limits<std::uint32_t>::max()};
    }

    std::string pageNameForListId(rs::core::ListId listId)
    {
      if (listId == allTracksListId())
      {
        return "all-tracks";
      }

      return "list-" + std::to_string(static_cast<unsigned long>(listId.value()));
    }

    struct StoredListNode final
    {
      rs::core::ListId id = rs::core::ListId{0};
      rs::core::ListId parentId = rootParentId();
      std::string name;
      bool isSmart = false;
      std::string localExpression;
    };

    std::string normalizeLibraryPath(std::filesystem::path const& path)
    {
      auto ec = std::error_code{};
      auto const canonicalPath = std::filesystem::weakly_canonical(path, ec);
      return ec ? path.lexically_normal().string() : canonicalPath.string();
    }

    bool hasTagName(std::vector<std::string_view> const& tagNames, std::string_view tag)
    {
      return std::ranges::find(tagNames, tag) != tagNames.end();
    }

    std::string tagChangeStatusMessage(std::size_t selectionCount, std::size_t addCount, std::size_t removeCount)
    {
      auto message = std::to_string(selectionCount);
      message += selectionCount == 1 ? " track updated" : " tracks updated";

      if (addCount == 0 && removeCount == 0)
      {
        return message;
      }

      message += ": ";

      if (addCount > 0)
      {
        message += std::to_string(addCount);
        message += addCount == 1 ? " tag added" : " tags added";
      }

      if (removeCount > 0)
      {
        if (addCount > 0)
        {
          message += ", ";
        }

        message += std::to_string(removeCount);
        message += removeCount == 1 ? " tag removed" : " tags removed";
      }

      return message;
    }

    app::ui::TrackColumnLayout trackColumnLayoutFromState(app::core::TrackViewState const& state)
    {
      auto layout = app::ui::defaultTrackColumnLayout();
      auto ordered = std::vector<app::ui::TrackColumnState>{};
      ordered.reserve(layout.columns.size());

      auto takeColumn = [&layout](app::ui::TrackColumn column) -> std::optional<app::ui::TrackColumnState>
      {
        auto const it =
          std::find_if(layout.columns.begin(),
                       layout.columns.end(),
                       [column](app::ui::TrackColumnState const& entry) { return entry.column == column; });
        if (it == layout.columns.end())
        {
          return std::nullopt;
        }

        return *it;
      };

      for (auto const& id : state.columnOrder)
      {
        auto const column = app::ui::trackColumnFromId(id);
        if (!column)
        {
          continue;
        }

        auto const existing =
          std::find_if(ordered.begin(),
                       ordered.end(),
                       [column](app::ui::TrackColumnState const& entry) { return entry.column == *column; });
        if (existing != ordered.end())
        {
          continue;
        }

        if (auto stateEntry = takeColumn(*column))
        {
          ordered.push_back(*stateEntry);
        }
      }

      for (auto const& entry : layout.columns)
      {
        auto const existing = std::find_if(ordered.begin(),
                                           ordered.end(),
                                           [column = entry.column](app::ui::TrackColumnState const& candidate)
                                           { return candidate.column == column; });
        if (existing == ordered.end())
        {
          ordered.push_back(entry);
        }
      }

      layout.columns = std::move(ordered);

      for (auto& entry : layout.columns)
      {
        auto const columnId = std::string{app::ui::trackColumnId(entry.column)};
        if (std::find(state.hiddenColumns.begin(), state.hiddenColumns.end(), columnId) != state.hiddenColumns.end())
        {
          entry.visible = false;
        }

        if (auto const width = state.columnWidths.find(columnId); width != state.columnWidths.end())
        {
          entry.width = width->second;
        }
      }

      return app::ui::normalizeTrackColumnLayout(std::move(layout));
    }

    app::core::TrackViewState trackViewStateFromLayout(app::ui::TrackColumnLayout const& layout)
    {
      auto normalized = app::ui::normalizeTrackColumnLayout(layout);
      auto state = app::core::TrackViewState{};

      for (auto const& entry : normalized.columns)
      {
        auto const columnId = std::string{app::ui::trackColumnId(entry.column)};
        state.columnOrder.push_back(columnId);

        if (!entry.visible)
        {
          state.hiddenColumns.push_back(columnId);
        }

        auto const definitionIt = std::find_if(app::ui::trackColumnDefinitions().begin(),
                                               app::ui::trackColumnDefinitions().end(),
                                               [column = entry.column](app::ui::TrackColumnDefinition const& definition)
                                               { return definition.column == column; });
        if (definitionIt != app::ui::trackColumnDefinitions().end() && entry.width != definitionIt->defaultWidth)
        {
          state.columnWidths.insert_or_assign(columnId, entry.width);
        }
      }

      return state;
    }
  }

  MainWindow::MainWindow()
    : _musicLibrary{nullptr}
    , _rowDataProvider{nullptr}
    , _allTrackIds{nullptr}
    , _coverArtWidget{nullptr}
    , _importDialog{nullptr}
    , _importWorker{nullptr}
    , _listTreeStore{nullptr}
    , _treeListModel{nullptr}
    , _listSelectionModel{nullptr}
    , _trackPages{}
    , _playbackBar{nullptr}
    , _playbackController{nullptr}
  {
    set_title("RockStudio");

    // Set default window size
    set_default_size(app::core::kDefaultWindowWidth, app::core::kDefaultWindowHeight);

    // Initialize cover art widget
    _coverArtWidget = std::make_unique<app::ui::CoverArtWidget>();

    setupPlayback();
    setupMenu();
    setupLayout();

    // Try to restore previous session
    loadSession();
  }

  MainWindow::~MainWindow()
  {
    if (_playbackTimer != 0)
    {
      g_source_remove(_playbackTimer);
      _playbackTimer = 0;
    }

    saveSession();

    // std::jthread auto-joins on destruction, no explicit join needed
  }

  void MainWindow::openLibrary()
  {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Open Music Library");

    // Show the dialog
    dialog->select_folder(*this,
                          [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result)
                          {
                            try
                            {
                              auto folder = dialog->select_folder_finish(result);

                              if (auto const folderPath = folder; folderPath)
                              {
                                std::filesystem::path path(folderPath->get_path());
                                APP_LOG_DEBUG("Selected folder: {}", path.string());

                                // Check if it's an existing library (contains data.mdb) or a new import
                                auto libPath = path / "data.mdb";
                                APP_LOG_DEBUG("libPath = {}", libPath.string());
                                APP_LOG_DEBUG("exists = {}", std::filesystem::exists(libPath));

                                if (std::filesystem::exists(libPath))
                                {
                                  openMusicLibrary(path);
                                }
                                else
                                {
                                  // Import new library
                                  importFilesFromPath(path);
                                }
                              }
                            }
                            catch (Glib::Error const& e)
                            {
                              // Folder selection was cancelled or failed - silently ignore
                              APP_LOG_ERROR("Error selecting folder: {}", e.what());
                            }
                          });
  }

  void MainWindow::openMusicLibrary(std::filesystem::path const& path)
  {
    APP_LOG_DEBUG("openMusicLibrary called with path: {}", path.string());

    try
    {
      auto musicLibrary = std::make_unique<rs::core::MusicLibrary>(path.string());
      APP_LOG_DEBUG("MusicLibrary created");

      auto rowDataProvider = std::make_shared<app::core::model::TrackRowDataProvider>(*musicLibrary);
      auto allTrackIds = std::make_unique<app::core::model::AllTrackIdsList>(musicLibrary->tracks());
      auto smartListEngine = std::make_unique<app::core::model::SmartListEngine>(*musicLibrary);

      auto txn = musicLibrary->readTransaction();
      allTrackIds->reloadFromStore(txn);

      // Replace the current page graph only after the new library is ready to use.
      clearTrackPages();

      _musicLibrary = std::move(musicLibrary);
      _rowDataProvider = std::move(rowDataProvider);
      _allTrackIds = std::move(allTrackIds);
      _smartListEngine = std::move(smartListEngine);

      if (_statusBar)
      {
        _statusBar->setTrackCount(_allTrackIds->size());
      }

      rebuildListPages(txn);

      // Show the "All Tracks" page
      _stack.set_visible_child(pageNameForListId(allTracksListId()));

      // Update window title
      set_title("RockStudio [" + path.string() + "]");

      // Save session
      saveSession();
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to open music library: {}", e.what());
    }
  }

  void MainWindow::scanDirectory(std::filesystem::path const& dir, std::vector<std::filesystem::path>& files)
  {
    static constexpr auto kSupportedExtensions = std::array{".mp3", ".m4a", ".flac"};

    try
    {
      for (auto const& entry : std::filesystem::recursive_directory_iterator(dir))
      {
        if (entry.is_regular_file())
        {
          auto ext = entry.path().extension().string();
          std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });

          if (std::ranges::find(kSupportedExtensions, ext) != kSupportedExtensions.end())
          {
            files.push_back(entry.path());
          }
        }
      }
    }
    catch (std::exception const& e)
    {
      // Directory scan failed - silently ignore
      APP_LOG_ERROR("Error scanning directory: {}", e.what());
    }
  }

  void MainWindow::importFiles()
  {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import Music Files");

    dialog->select_folder(
      *this,
      [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result)
      {
        try
        {
          auto folder = dialog->select_folder_finish(result);

          if (!folder)
          {
            return;
          }

          std::string pathStr = folder->get_path();
          std::filesystem::path path(pathStr);
          APP_LOG_INFO("Importing from: {}", pathStr);

          // If no library exists, create one at the import path

          if (!_musicLibrary)
          {
            _musicLibrary = std::make_unique<rs::core::MusicLibrary>(pathStr);
            set_title("RockStudio [" + pathStr + "]");
          }

          // Scan for music files
          std::vector<std::filesystem::path> files;
          scanDirectory(path, files);

          if (files.empty())
          {
            APP_LOG_ERROR("No music files found");
            return;
          }

          // Create progress dialog owned by MainWindow (stored as member)
          _importDialog = std::make_unique<ImportProgressDialog>(static_cast<int>(files.size()), *this);
          auto* dialogPtr = _importDialog.get();
          _importDialog->signal_response().connect([dialogPtr](int /*responseId*/) { dialogPtr->close(); });

          // Create worker - owned by MainWindow
          _importWorker = std::make_unique<app::core::ImportWorker>(
            *_musicLibrary,
            files,
            [this, dialogPtr](std::filesystem::path const& path, int index)
            {
              // Progress callback - marshal to main thread
              Glib::MainContext::get_default()->invoke(
                [this, dialogPtr, path, index]()
                {
                  if (dialogPtr)
                  {
                    dialogPtr->onNewTrack(path.string(), index);
                  }

                  auto fraction = static_cast<double>(index) / static_cast<double>(_importWorker->fileCount());
                  updateImportProgress(fraction, "Importing: " + path.filename().string());

                  return false;
                });
            },
            [this, dialogPtr]()
            {
              // Finished callback - marshal to main thread
              Glib::MainContext::get_default()->invoke(
                [this, dialogPtr]()
                {
                  if (dialogPtr)
                  {
                    dialogPtr->ready();
                  }

                  updateImportProgress(1.0, "Import complete");

                  return false;
                });
            });

          // Run in background thread - owned and joined on window destruction
          auto* workerPtr = _importWorker.get();

          if (_importThread.joinable())
          {
            _importThread.join();
          }

          _importThread = std::jthread(
            [this, workerPtr]([[maybe_unused]] std::stop_token stoken)
            {
              workerPtr->run();
              // After import completes, notify observers incrementally
              Glib::MainContext::get_default()->invoke(
                [this, workerPtr]()
                {
                  auto const& result = workerPtr->result();
                  for (auto const trackId : result.insertedIds)
                  {
                    _rowDataProvider->invalidateFull(trackId);
                    _allTrackIds->notifyInserted(trackId);
                  }
                  return false;
                });
            });

          _importDialog->show();
        }
        catch (Glib::Error const& e)
        {
          // Folder selection was cancelled or failed - silently ignore
          APP_LOG_ERROR("Error selecting folder: {}", e.what());
        }
      });
  }

  void MainWindow::importFilesFromPath(std::filesystem::path const& path)
  {
    // Clear existing track pages first (adapters hold pointers to old _allTrackIds)
    clearTrackPages();

    // Create new music library at the path
    _musicLibrary = std::make_unique<rs::core::MusicLibrary>(path.string());

    // Initialize row data provider
    _rowDataProvider = std::make_shared<app::core::model::TrackRowDataProvider>(*_musicLibrary);

    // Initialize AllTrackIdsList
    _allTrackIds = std::make_unique<app::core::model::AllTrackIdsList>(_musicLibrary->tracks());

    // Initialize SmartListEngine for smart lists
    _smartListEngine = std::make_unique<app::core::model::SmartListEngine>(*_musicLibrary);

    // Scan for music files
    std::vector<std::filesystem::path> files;
    scanDirectory(path, files);

    if (files.empty())
    {
      APP_LOG_ERROR("No music files found");
      return;
    }

    // Show progress dialog
    _importDialog = std::make_unique<ImportProgressDialog>(static_cast<int>(files.size()), *this);
    auto* dialogPtr = _importDialog.get();
    _importDialog->signal_response().connect([dialogPtr](int /*responseId*/) { dialogPtr->close(); });

    // Create worker - owned by MainWindow
    _importWorker = std::make_unique<app::core::ImportWorker>(
      *_musicLibrary,
      files,
      [this, dialogPtr](std::filesystem::path const& path, int index)
      {
        Glib::MainContext::get_default()->invoke(
          [this, dialogPtr, path, index]
          {
            dialogPtr->onNewTrack(path.string(), index);
            auto fraction = static_cast<double>(index) / static_cast<double>(_importWorker->fileCount());
            updateImportProgress(fraction, "Importing: " + path.filename().string());
            return false;
          });
      },
      [this, dialogPtr]
      {
        Glib::MainContext::get_default()->invoke(
          [this, dialogPtr]
          {
            dialogPtr->ready();
            updateImportProgress(1.0, "Import complete");
            return false;
          });
      });

    // Run in background thread - owned and joined on window destruction
    auto* workerPtr = _importWorker.get();

    if (_importThread.joinable())
    {
      _importThread.join();
    }

    _importThread = std::jthread(
      [this, workerPtr]([[maybe_unused]] std::stop_token stoken)
      {
        workerPtr->run();
        // After import completes, notify observers incrementally
        Glib::MainContext::get_default()->invoke(
          [this, workerPtr]()
          {
            auto const& result = workerPtr->result();
            for (auto const trackId : result.insertedIds)
            {
              _rowDataProvider->invalidateFull(trackId);
              _allTrackIds->notifyInserted(trackId);
            }
            return false;
          });
      });

    _importDialog->show();
  }

  void MainWindow::openNewListDialog(rs::core::ListId parentListId)
  {
    if (!_musicLibrary)
    {
      return;
    }

    // Determine the parent membership list
    app::core::model::TrackIdList* parentMembershipList = nullptr;
    if (parentListId == allTracksListId())
    {
      // Use All Tracks as source
      parentMembershipList = _allTrackIds.get();
    }
    else
    {
      // Find the parent's membership list from track pages
      if (auto const it = _trackPages.find(parentListId); it != _trackPages.end() && it->second.membershipList)
      {
        parentMembershipList = it->second.membershipList.get();
      }
      else
      {
        // Fallback to All Tracks if parent not found
        parentMembershipList = _allTrackIds.get();
      }
    }

    auto* dialog =
      Gtk::make_managed<SmartListDialog>(*this, *_musicLibrary, *_allTrackIds, *parentMembershipList, parentListId);

    dialog->signal_response().connect(
      [this, dialog](int responseId)
      {
        if (responseId == Gtk::ResponseType::OK)
        {
          auto const draft = dialog->draft();
          if (draft.listId != rs::core::ListId{0})
          {
            updateList(draft);
          }
          else
          {
            createList(draft);
          }
        }

        dialog->close();
      });

    dialog->present();
  }

  void MainWindow::openNewSmartListDialog()
  {
    // Smart selection: if a non-All-Tracks list is selected, use it as parent; otherwise use root
    auto parentListId = rootParentId();

    if (auto const selected = _listSelectionModel ? _listSelectionModel->get_selected() : GTK_INVALID_LIST_POSITION;
        _treeListModel && selected != GTK_INVALID_LIST_POSITION && selected != 0)
    {
      if (auto model = _listSelectionModel->get_model())
      {
        if (auto item = model->get_object(selected))
        {
          if (auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item); treeListRow != nullptr)
          {
            if (auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item()); node != nullptr)
            {
              parentListId = node->getListId();
            }
          }
        }
      }
    }

    openNewListDialog(parentListId);
  }

  bool MainWindow::listHasChildren(rs::core::ListId listId) const
  {
    auto it = _nodesById.find(listId);

    if (it == _nodesById.end())
    {
      return false;
    }

    return it->second->hasChildren();
  }

  void MainWindow::setupMenu()
  {
    // Create menu model
    auto menuModel = Gio::Menu::create();

    // File menu
    auto fileMenu = Gio::Menu::create();
    fileMenu->append("Open Library", "win.open-library");
    fileMenu->append("Import Files", "win.import-files");
    fileMenu->append("Quit", "app.quit");
    menuModel->append_submenu("File", fileMenu);

    // Help menu (placeholder)
    auto helpMenu = Gio::Menu::create();
    helpMenu->append("About", "app.about");
    menuModel->append_submenu("Help", helpMenu);

    // Set up menu bar
    _menuBar.set_menu_model(menuModel);

    // Create actions
    auto openAction = Gio::SimpleAction::create("open-library");
    openAction->signal_activate().connect([this]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
                                          { openLibrary(); });
    add_action(openAction);

    auto importAction = Gio::SimpleAction::create("import-files");
    importAction->signal_activate().connect([this]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
                                            { importFiles(); });
    add_action(importAction);

    _newListAction = Gio::SimpleAction::create("new-list");
    _newListAction->signal_activate().connect([this]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
                                              { openNewSmartListDialog(); });
    _newListAction->set_enabled(false);
    add_action(_newListAction);

    _deleteListAction = Gio::SimpleAction::create("delete-list");
    _deleteListAction->signal_activate().connect([this]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
                                                 { onDeleteList(); });
    _deleteListAction->set_enabled(false);
    add_action(_deleteListAction);

    _editListAction = Gio::SimpleAction::create("edit-list");
    _editListAction->signal_activate().connect([this]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
                                               { onEditList(); });
    _editListAction->set_enabled(false);
    add_action(_editListAction);
  }

  void MainWindow::setupLayout()
  {
    // Set up horizontal paned (split view)
    _paned.set_orientation(Gtk::Orientation::HORIZONTAL);

    // Left side: vertical box (like Qt's verticalLayout)
    _leftBox.set_orientation(Gtk::Orientation::VERTICAL);
    _leftBox.set_hexpand(true);
    _leftBox.set_vexpand(true);
    _leftBox.set_homogeneous(false);

    // Create empty tree store for sidebar (will be populated by rebuildListPages)
    _listTreeStore = Gio::ListStore<ListTreeNode>::create();

    // List view for the sidebar
    auto factory = Gtk::SignalListItemFactory::create();
    factory->signal_setup().connect(
      [this](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        rowBox->set_halign(Gtk::Align::FILL);
        rowBox->set_hexpand(true);
        rowBox->set_margin_start(6);
        rowBox->set_margin_end(6);
        rowBox->set_margin_top(3);
        rowBox->set_margin_bottom(3);

        auto* expander = Gtk::make_managed<Gtk::TreeExpander>();
        rowBox->append(*expander);

        auto* label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        rowBox->append(*label);

        auto* filterLabel = Gtk::make_managed<Gtk::Label>("");
        filterLabel->set_halign(Gtk::Align::START);
        filterLabel->add_css_class("dim-label");
        filterLabel->set_margin_start(6);
        filterLabel->set_ellipsize(Pango::EllipsizeMode::END);
        filterLabel->set_hexpand(true);
        rowBox->append(*filterLabel);

        auto clickController = Gtk::GestureClick::create();
        clickController->set_button(GDK_BUTTON_SECONDARY);
        clickController->signal_pressed().connect(
          [this, listItem, rowBox](int /*nPress*/, double x, double y)
          {
            if (auto const position = listItem->get_position(); position != GTK_INVALID_LIST_POSITION)
            {
              _listSelectionModel->set_selected(position);
            }

            auto point =
              rowBox->compute_point(_listView, Gdk::Graphene::Point(static_cast<float>(x), static_cast<float>(y)));

            if (!point)
            {
              return;
            }

            auto rect = Gdk::Rectangle(static_cast<int>(point->get_x()), static_cast<int>(point->get_y()), 1, 1);
            showListContextMenu(_listView, rect);
          });

        rowBox->add_controller(clickController);
        listItem->set_child(*rowBox);
      });
    factory->signal_bind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(listItem->get_item());

        if (!treeListRow)
        {
          return;
        }

        auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item());

        if (!node)
        {
          return;
        }

        auto row = node->getRow();
        auto box = dynamic_cast<Gtk::Box*>(listItem->get_child());
        auto expander = box ? dynamic_cast<Gtk::TreeExpander*>(box->get_first_child()) : nullptr;
        auto label = expander ? dynamic_cast<Gtk::Label*>(expander->get_next_sibling()) : nullptr;
        auto filterLabel = label ? dynamic_cast<Gtk::Label*>(label->get_next_sibling()) : nullptr;

        if (expander)
        {
          expander->set_list_row(treeListRow);
        }

        if (row && label)
        {
          // Gtk::TreeExpander handles indentation automatically
          label->set_text(row->getName());

          if (filterLabel)
          {
            auto const filter = row->getFilter();
            if (!filter.empty())
            {
              filterLabel->set_text("[" + filter + "]");
              filterLabel->set_visible(true);
            }
            else
            {
              filterLabel->set_text("");
              filterLabel->set_visible(false);
            }
          }
        }
      });

    _listView.set_factory(factory);
    _listView.set_halign(Gtk::Align::FILL);
    _listView.set_valign(Gtk::Align::FILL);
    _listView.set_hexpand(true);
    _listView.set_vexpand(true);

    auto listContextMenuModel = Gio::Menu::create();
    listContextMenuModel->append("New List", "win.new-list");
    listContextMenuModel->append("Edit List", "win.edit-list");
    listContextMenuModel->append("Delete List", "win.delete-list");
    _listContextMenu.set_menu_model(listContextMenuModel);
    _listContextMenu.set_has_arrow(false);
    _listContextMenu.set_parent(_listView);

    // Scrolled window for list
    _listScrolledWindow.set_child(_listView);
    _listScrolledWindow.set_vexpand(true);

    // Cover art widget - matches Qt's CoverArtLabel (vsizetype=Maximum, min 50x50)
    _coverArtWidget->set_valign(Gtk::Align::END);
    _coverArtWidget->set_halign(Gtk::Align::FILL);
    _coverArtWidget->set_size_request(50, 50);
    _coverArtWidget->set_vexpand(false);
    _coverArtWidget->set_hexpand(false);

    // Add widgets to left box
    _leftBox.append(_listScrolledWindow);
    _leftBox.append(*_coverArtWidget);

    // Right side: stack for pages
    _stack.set_hexpand(true);
    _stack.set_vexpand(true);

    // Add placeholder page
    auto* placeholderLabel = Gtk::make_managed<Gtk::Label>("Select a library or import tracks");
    placeholderLabel->set_halign(Gtk::Align::CENTER);
    placeholderLabel->set_valign(Gtk::Align::CENTER);
    _stack.add(*placeholderLabel, "welcome", "Welcome");

    // Add to paned
    _paned.set_start_child(_leftBox);
    _paned.set_end_child(_stack);

    // Set paned behavior to match Qt's 1:2 stretch ratio
    // Left side (list) can resize but not shrink, right side gets more space
    _paned.set_resize_start_child(true);
    _paned.set_shrink_start_child(false);
    _paned.set_resize_end_child(true);
    _paned.set_shrink_end_child(false);

    // Set initial position to give 1/3 to left, 2/3 to right
    constexpr std::int32_t kPanedInitialPosition = 330;
    _paned.set_position(kPanedInitialPosition);

    // Set up the main layout
    auto* mainBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    mainBox->append(_menuBar);

    if (_playbackBar)
    {
      mainBox->append(*_playbackBar);
    }

    mainBox->append(_paned);

    // Status bar at bottom
    _statusBar = std::make_unique<StatusBar>();

    auto* statusSeparator = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    mainBox->append(*statusSeparator);
    mainBox->append(*_statusBar);

    // Set as window child
    set_child(*mainBox);
  }

  void MainWindow::showListContextMenu(Gtk::ListView& listView, Gdk::Rectangle const& rect)
  {
    (void)listView;

    auto const hasLibrary = static_cast<bool>(_musicLibrary);

    auto canDelete = false;
    auto canEdit = false;

    if (auto const selected = _listSelectionModel ? _listSelectionModel->get_selected() : GTK_INVALID_LIST_POSITION;
        hasLibrary && _treeListModel && selected != GTK_INVALID_LIST_POSITION && selected != 0)
    {
      if (auto item = _listSelectionModel->get_selected_item())
      {
        if (auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item))
        {
          if (auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item()))
          {
            canDelete = !listHasChildren(node->getListId());
            canEdit = true;
          }
        }
      }
    }

    if (_newListAction)
    {
      _newListAction->set_enabled(hasLibrary);
    }

    if (_deleteListAction)
    {
      _deleteListAction->set_enabled(canDelete);
    }

    if (_editListAction)
    {
      _editListAction->set_enabled(canEdit);
    }

    _listContextMenu.set_pointing_to(rect);
    _listContextMenu.popup();
  }

  void MainWindow::createList(app::core::model::ListDraft const& draft)
  {
    if (!_musicLibrary)
    {
      APP_LOG_ERROR("No music library open");
      return;
    }

    auto txn = _musicLibrary->writeTransaction();

    // Build the list payload
    auto builder =
      rs::core::ListBuilder::createNew().name(draft.name).description(draft.description).parentId(draft.parentId);

    if (draft.kind == app::core::model::ListKind::Smart)
    {
      builder.filter(draft.expression);
    }
    else
    {
      for (auto id : draft.trackIds)
      {
        builder.tracks().add(id);
      }
    }

    auto payload = builder.serialize();

    // Create the list in the store
    auto [listId, view] = _musicLibrary->lists().writer(txn).create(payload);

    txn.commit();

    // Refresh the lists
    auto readTxn = _musicLibrary->readTransaction();
    rebuildListPages(readTxn);

    // Find and select the newly created list
    if (_treeListModel)
    {
      auto const itemCount = _treeListModel->get_n_items();

      for (guint index = 0; index < itemCount; ++index)
      {
        if (auto item = _treeListModel->get_object(index); item != nullptr)
        {
          if (auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item); treeListRow != nullptr)
          {
            if (auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item()); node != nullptr)
            {
              if (node->getListId() == listId)
              {
                _listSelectionModel->set_selected(index);
                break;
              }
            }
          }
        }
      }
    }
  }

  void MainWindow::updateList(app::core::model::ListDraft const& draft)
  {
    if (!_musicLibrary)
    {
      APP_LOG_ERROR("No music library open");
      return;
    }

    auto txn = _musicLibrary->writeTransaction();

    auto builder =
      rs::core::ListBuilder::createNew().name(draft.name).description(draft.description).parentId(draft.parentId);

    if (draft.kind == app::core::model::ListKind::Smart)
    {
      builder.filter(draft.expression);
    }
    else
    {
      for (auto id : draft.trackIds)
      {
        builder.tracks().add(id);
      }
    }

    auto payload = builder.serialize();

    _musicLibrary->lists().writer(txn).update(draft.listId, payload);

    txn.commit();

    // Refresh the list page
    auto readTxn = _musicLibrary->readTransaction();
    auto reader = _musicLibrary->lists().reader(readTxn);

    if (auto view = reader.get(draft.listId))
    {
      buildPageForStoredList(draft.listId, *view);
    }
  }

  void MainWindow::onEditList()
  {
    if (!_musicLibrary)
    {
      return;
    }

    auto const position = _listSelectionModel->get_selected();

    if (position == GTK_INVALID_LIST_POSITION)
    {
      return;
    }

    // Don't allow editing "All Tracks" (position 0)
    if (position == 0)
    {
      return;
    }

    auto item = _listSelectionModel->get_selected_item();

    if (!item)
    {
      return;
    }

    auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item);

    if (!treeListRow)
    {
      return;
    }

    auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item());

    if (!node)
    {
      return;
    }

    openEditListDialog(node->getListId());
  }

  void MainWindow::openEditListDialog(rs::core::ListId listId)
  {
    if (!_musicLibrary)
    {
      return;
    }

    auto readTxn = _musicLibrary->readTransaction();
    auto reader = _musicLibrary->lists().reader(readTxn);
    auto view = reader.get(listId);

    if (!view)
    {
      return;
    }

    // Determine the parent membership list for the preview
    app::core::model::TrackIdList* parentMembershipList = nullptr;
    auto const parentId = view->parentId();
    if (parentId == allTracksListId())
    {
      parentMembershipList = _allTrackIds.get();
    }
    else
    {
      if (auto const it = _trackPages.find(parentId); it != _trackPages.end() && it->second.membershipList)
      {
        parentMembershipList = it->second.membershipList.get();
      }
      else
      {
        parentMembershipList = _allTrackIds.get();
      }
    }

    auto* dialog =
      Gtk::make_managed<SmartListDialog>(*this, *_musicLibrary, *_allTrackIds, *parentMembershipList, view->parentId());

    dialog->populate(listId, *view);

    dialog->signal_response().connect(
      [this, dialog](int responseId)
      {
        if (responseId == Gtk::ResponseType::OK)
        {
          auto const draft = dialog->draft();
          if (draft.listId != rs::core::ListId{0})
          {
            updateList(draft);
          }
        }

        dialog->close();
      });

    dialog->present();
  }

  void MainWindow::onDeleteList()
  {
    if (!_musicLibrary)
    {
      return;
    }

    auto position = _listSelectionModel->get_selected();

    if (position == GTK_INVALID_LIST_POSITION)
    {
      return;
    }

    // Don't allow deleting "All Tracks" (position 0)
    if (position == 0)
    {
      return;
    }

    auto item = _listSelectionModel->get_selected_item();

    if (!item)
    {
      return;
    }

    auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item);

    if (!treeListRow)
    {
      return;
    }

    auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item());

    if (!node)
    {
      return;
    }

    auto listId = node->getListId();

    if (listHasChildren(listId))
    {
      APP_LOG_ERROR("Cannot delete a list that still has child lists");
      return;
    }

    // Delete the list from the library
    auto txn = _musicLibrary->writeTransaction();
    _musicLibrary->lists().writer(txn).del(listId);
    txn.commit();

    // Refresh lists
    auto readTxn = _musicLibrary->readTransaction();
    rebuildListPages(readTxn);
    _listSelectionModel->set_selected(0);
  }

  void MainWindow::clearTrackPages()
  {
    while (!_trackPages.empty())
    {
      auto it = std::prev(_trackPages.end());

      if (it->second.page)
      {
        _stack.remove(*it->second.page);
      }

      _trackPages.erase(it);
    }
  }

  void MainWindow::rebuildListPages(rs::lmdb::ReadTransaction& txn)
  {
    APP_LOG_DEBUG("rebuildListPages called");
    // Clear existing track pages
    clearTrackPages();

    // Build the tree structure
    buildListTree(txn);

    // Build track pages for "All Tracks" and all stored lists
    buildPageForAllTracks();

    auto reader = _musicLibrary->lists().reader(txn);

    for (auto const& [id, listView] : reader)
    {
      (void)id;
      // Create track view page for this list
      buildPageForStoredList(id, listView);
    }

    // Set up selection model after tree is built
    if (_treeListModel)
    {
      _listSelectionModel = Gtk::SingleSelection::create(_treeListModel);
      _listSelectionModel->signal_selection_changed().connect(
        sigc::mem_fun(*this, &MainWindow::onListSelectionChanged));
      _listView.set_model(_listSelectionModel);
    }

    // Set up track context menu for all track pages
    setupTrackContextMenu();
  }

  void MainWindow::buildListTree(rs::lmdb::ReadTransaction& txn)
  {
    // Clear existing tree store and lookup map
    _nodesById.clear();

    // Create new tree store
    _listTreeStore = Gio::ListStore<ListTreeNode>::create();

    auto reader = _musicLibrary->lists().reader(txn);
    auto nodes = std::map<rs::core::ListId, StoredListNode>{};

    for (auto const& [id, listView] : reader)
    {
      nodes.emplace(id,
                    StoredListNode{
                      .id = id,
                      .parentId = listView.parentId(),
                      .name = std::string(listView.name()),
                      .isSmart = listView.isSmart(),
                      .localExpression = std::string(listView.filter()),
                    });
    }

    // Build children map
    auto children = std::map<rs::core::ListId, std::vector<rs::core::ListId>>{};

    for (auto const& [id, node] : nodes)
    {
      if (node.parentId != rootParentId() && node.parentId != id && nodes.contains(node.parentId))
      {
        children[node.parentId].push_back(id);
      }
    }

    // Create tree nodes for all stored lists
    for (auto const& [id, node] : nodes)
    {
      auto listRow = ListRow::create(id, node.parentId, 0, node.isSmart, node.name, node.localExpression);
      auto treeNode = ListTreeNode::create(listRow);
      _nodesById[id] = treeNode;
    }

    // Create "All Tracks" root node
    auto allRow = ListRow::create(allTracksListId(), rootParentId(), 0, false, "All Tracks");
    auto allTracksNode = ListTreeNode::create(allRow);
    _nodesById[allTracksListId()] = allTracksNode;

    // Attach children to parents
    for (auto const& [id, node] : nodes)
    {
      auto childNode = _nodesById[id];
      auto parentId = node.parentId;

      if (auto parentNodeIt = _nodesById.find(parentId); parentNodeIt != _nodesById.end())
      {
        parentNodeIt->second->getChildren()->append(childNode);
        childNode->setParent(parentNodeIt->second.get());
      }
      else
      {
        // Parent not found, attach to All Tracks root
        allTracksNode->getChildren()->append(childNode);
        childNode->setParent(allTracksNode.get());
      }
    }

    // Add allTracksNode to the tree store
    _listTreeStore->append(allTracksNode);

    // Create Gtk::TreeListModel wrapping the store
    // Return nullptr for leaf nodes so GTK doesn't show an expander
    _treeListModel = Gtk::TreeListModel::create(
      _listTreeStore,
      [this](Glib::RefPtr<Glib::ObjectBase> const& item) -> Glib::RefPtr<Gio::ListModel>
      {
        auto node = std::dynamic_pointer_cast<ListTreeNode>(item);

        if (!node || !node->hasChildren())
        {
          return nullptr;
        }

        return node->getChildren();
      },
      false,
      true);
  }

  void MainWindow::buildPageForAllTracks()
  {
    // Create adapter using the shared _allTrackIds (not a page-local copy)
    // _allTrackIds is the authoritative source that import/tag notifies update
    auto adapter = std::make_shared<TrackListAdapter>(*_allTrackIds, _rowDataProvider);
    // Manually trigger rebuild since notifyReset was called before adapter was attached
    adapter->onReset();
    auto trackPage = std::make_unique<TrackViewPage>(adapter, _trackColumnLayoutModel);

    auto pageId = pageNameForListId(allTracksListId());
    _stack.add(*trackPage, pageId, "All Tracks");

    // Connect selection to cover art update
    trackPage->signalSelectionChanged().connect(
      [this, trackPagePtr = trackPage.get()]()
      {
        auto ids = trackPagePtr->getSelectedTrackIds();
        updateCoverArt(ids);
        onTrackSelectionChanged();
      });
    trackPage->signalContextMenuRequested().connect([this, trackPagePtr = trackPage.get()](double x, double y)
                                                    { showTrackContextMenu(*trackPagePtr, x, y); });
    trackPage->signalTagEditRequested().connect(
      [this, trackPagePtr = trackPage.get()](std::vector<rs::core::TrackId> ids, double x, double y)
      { showTagEditor(*trackPagePtr, ids, x, y); });

    // Connect track activation to playback
    bindTrackPagePlayback(*trackPage);

    TrackPageContext ctx;
    ctx.membershipList = nullptr; // All-tracks page observes shared _allTrackIds, no ownership
    ctx.adapter = std::move(adapter);
    ctx.page = std::move(trackPage);
    _trackPages[allTracksListId()] = std::move(ctx);
  }

  void MainWindow::buildPageForStoredList(rs::core::ListId listId, rs::core::ListView const& view)
  {
    // Get display name from payload
    std::string listName;

    if (auto const name = view.name(); !name.empty())
    {
      listName = std::string(name);
    }
    else
    {
      listName = "<Unnamed List>";
    }

    // Create appropriate membership list based on smart vs manual
    std::unique_ptr<app::core::model::TrackIdList> membershipList;

    if (view.isSmart())
    {
      auto* sourceList = static_cast<app::core::model::TrackIdList*>(_allTrackIds.get());

      if (!view.isRootParent())
      {
        auto const sourceIt = _trackPages.find(view.parentId());

        if (sourceIt != _trackPages.end() && sourceIt->second.membershipList)
        {
          sourceList = sourceIt->second.membershipList.get();
        }
        else
        {
          APP_LOG_ERROR("Missing source list for smart list {}, falling back to All Tracks", listId.value());
        }
      }

      // Smart list - use FilteredTrackIdList
      auto filtered =
        std::make_unique<app::core::model::FilteredTrackIdList>(*sourceList, *_musicLibrary, *_smartListEngine);

      // Set expression from filter stored in payload
      auto expr = view.filter();
      filtered->setExpression(std::string(expr));
      filtered->reload();
      membershipList = std::move(filtered);
    }
    else
    {
      // Manual list - use ManualTrackIdList, observes _allTrackIds for updates/removes
      auto manual = std::make_unique<app::core::model::ManualTrackIdList>(view, _allTrackIds.get());
      membershipList = std::move(manual);
    }

    // Create adapter
    auto adapter = std::make_shared<TrackListAdapter>(*membershipList, _rowDataProvider);
    // Prime the model with the membership list contents because the list was populated
    // before the adapter attached as an observer.
    adapter->onReset();

    // Create track page
    auto trackPage = std::make_unique<TrackViewPage>(adapter, _trackColumnLayoutModel);

    auto pageId = pageNameForListId(listId);
    _stack.add(*trackPage, pageId, listName);

    // Connect selection to cover art update
    trackPage->signalSelectionChanged().connect(
      [this, trackPagePtr = trackPage.get()]()
      {
        auto ids = trackPagePtr->getSelectedTrackIds();
        updateCoverArt(ids);
        onTrackSelectionChanged();
      });
    trackPage->signalContextMenuRequested().connect([this, trackPagePtr = trackPage.get()](double x, double y)
                                                    { showTrackContextMenu(*trackPagePtr, x, y); });
    trackPage->signalTagEditRequested().connect(
      [this, trackPagePtr = trackPage.get()](std::vector<rs::core::TrackId> ids, double x, double y)
      { showTagEditor(*trackPagePtr, ids, x, y); });

    // Connect track activation to playback
    bindTrackPagePlayback(*trackPage);

    // Create playlist exporter for this list
    auto playlistDir = _musicLibrary->rootPath() / "playlist";

    if (!std::filesystem::exists(playlistDir))
    {
      std::filesystem::create_directories(playlistDir);
    }

    auto playlistPath = playlistDir / (listName + ".m3u");
    auto exporter = std::make_unique<app::services::PlaylistExporter>(
      *membershipList, *_rowDataProvider, _musicLibrary->rootPath(), playlistPath);

    TrackPageContext ctx;
    ctx.membershipList = std::move(membershipList);
    ctx.adapter = std::move(adapter);
    ctx.page = std::move(trackPage);
    ctx.exporter = std::move(exporter);
    _trackPages[listId] = std::move(ctx);
  }

  void MainWindow::setupTrackContextMenu()
  {
    if (_trackTagAddAction && _trackTagRemoveAction)
    {
      return;
    }

    auto const stringType = Glib::VariantType("s");

    _trackTagAddAction = Gio::SimpleAction::create("track-tag-add", stringType);
    _trackTagAddAction->signal_activate().connect(
      [this](Glib::VariantBase const& parameter)
      { addTagToCurrentSelection(Glib::VariantBase::cast_dynamic<Glib::Variant<std::string>>(parameter).get()); });
    add_action(_trackTagAddAction);

    _trackTagRemoveAction = Gio::SimpleAction::create("track-tag-remove", stringType);
    _trackTagRemoveAction->signal_activate().connect(
      [this](Glib::VariantBase const& parameter)
      { removeTagFromCurrentSelection(Glib::VariantBase::cast_dynamic<Glib::Variant<std::string>>(parameter).get()); });
    add_action(_trackTagRemoveAction);
  }

  void MainWindow::showTrackContextMenu(TrackViewPage& page, double x, double y)
  {
    if (!_musicLibrary)
    {
      return;
    }

    auto selectedIds = page.getSelectedTrackIds();

    if (selectedIds.empty())
    {
      return;
    }

    // Create TagPopover with the selected track IDs
    auto* tagPopover = new TagPopover(*_musicLibrary, selectedIds);

    // Connect signal to apply tag changes
    tagPopover->signalTagsChanged().connect(
      [this](std::vector<std::string> const& tagsToAdd, std::vector<std::string> const& tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    // Show the popover anchored to the right-click position
    page.showTagPopover(*tagPopover, x, y);
  }

  void MainWindow::showTagEditor(TrackViewPage& page, std::vector<rs::core::TrackId> selectedIds, double x, double y)
  {
    if (!_musicLibrary)
    {
      return;
    }

    if (selectedIds.empty())
    {
      return;
    }

    // Create TagPopover with the selected track IDs
    auto* tagPopover = new TagPopover(*_musicLibrary, selectedIds);

    // Connect signal to apply tag changes
    tagPopover->signalTagsChanged().connect(
      [this](std::vector<std::string> const& tagsToAdd, std::vector<std::string> const& tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    // Show popover at mouse position
    tagPopover->set_parent(page.getColumnView());
    Gdk::Rectangle rect{static_cast<int>(x), static_cast<int>(y), 1, 1};
    tagPopover->set_pointing_to(rect);
    tagPopover->popup();
  }

  void MainWindow::addTagToCurrentSelection(std::string const& tag)
  {
    applyTagChangeToCurrentSelection({tag}, {});
  }

  void MainWindow::removeTagFromCurrentSelection(std::string const& tag)
  {
    applyTagChangeToCurrentSelection({}, {tag});
  }

  void MainWindow::applyTagChangeToCurrentSelection(std::vector<std::string> const& tagsToAdd,
                                                    std::vector<std::string> const& tagsToRemove)
  {
    if (!_musicLibrary || !_rowDataProvider || !_allTrackIds)
    {
      return;
    }

    auto* ctx = currentVisibleTrackPageContext();
    if (!ctx)
    {
      return;
    }

    auto selectedIds = ctx->page->getSelectedTrackIds();

    if (selectedIds.empty() || (tagsToAdd.empty() && tagsToRemove.empty()))
    {
      return;
    }

    auto txn = _musicLibrary->writeTransaction();
    auto writer = _musicLibrary->tracks().writer(txn);
    auto& dict = _musicLibrary->dictionary();

    for (auto const trackId : selectedIds)
    {
      auto const optView = writer.get(trackId, rs::core::TrackStore::Reader::LoadMode::Hot);
      if (!optView)
      {
        continue;
      }

      auto builder = rs::core::TrackBuilder::fromView(*optView, dict);

      for (auto const& tag : tagsToRemove)
      {
        builder.tags().remove(tag);
      }

      for (auto const& tag : tagsToAdd)
      {
        if (!hasTagName(builder.tags().names(), tag))
        {
          builder.tags().add(tag);
        }
      }

      auto hotData = builder.serializeHot(txn, dict);
      writer.updateHot(trackId, hotData);
    }

    txn.commit();

    for (auto const trackId : selectedIds)
    {
      _rowDataProvider->invalidateHot(trackId);
      if (ctx->membershipList)
      {
        ctx->membershipList->notifyTrackDataChanged(trackId);
      }
      else
      {
        // All Tracks page uses _allTrackIds directly
        _allTrackIds->notifyTrackDataChanged(trackId);
      }
    }

    showStatusMessage(tagChangeStatusMessage(selectedIds.size(), tagsToAdd.size(), tagsToRemove.size()));
  }

  void MainWindow::showStatusMessage(std::string const& message)
  {
    if (_statusBar)
    {
      _statusBar->showMessage(message);
    }
  }

  void MainWindow::onListSelectionChanged([[maybe_unused]] std::uint32_t position,
                                          [[maybe_unused]] std::uint32_t nItems)
  {
    if (auto const selected = _listSelectionModel ? _listSelectionModel->get_selected() : GTK_INVALID_LIST_POSITION;
        selected == GTK_INVALID_LIST_POSITION)
    {
      return;
    }

    auto item = _listSelectionModel->get_selected_item();

    if (!item)
    {
      return;
    }

    auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item);

    if (!treeListRow)
    {
      return;
    }

    auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item());

    if (!node)
    {
      return;
    }

    auto listId = node->getListId();

    // Switch to the corresponding stack page
    _stack.set_visible_child(pageNameForListId(listId));
  }

  void MainWindow::updateCoverArt(std::vector<rs::core::TrackId> const& selectedIds)
  {
    if (!_musicLibrary || selectedIds.empty())
    {
      // Clear cover art - use default
      _coverArtWidget->clearCover();
      return;
    }

    // Get the first selected track
    auto trackId = selectedIds.front();

    // Look up cover art ID via provider
    auto coverArtId = _rowDataProvider->getCoverArtId(trackId);

    if (!coverArtId)
    {
      _coverArtWidget->clearCover();
      return;
    }

    // Load cover art from ResourceStore and display
    rs::lmdb::ReadTransaction txn(_musicLibrary->readTransaction());
    auto resourceReader = _musicLibrary->resources().reader(txn);
    auto optBytes = resourceReader.get(*coverArtId);

    if (optBytes)
    {
      auto artBytes = std::vector<std::byte>{optBytes->begin(), optBytes->end()};
      _coverArtWidget->setCoverFromBytes(artBytes);
    }
    else
    {
      _coverArtWidget->clearCover();
    }
  }

  void MainWindow::notifyTracksInserted(std::vector<rs::core::TrackId> const& ids)
  {
    for (auto id : ids)
    {
      _allTrackIds->notifyInserted(id);
    }
  }

  void MainWindow::notifyTracksUpdated(std::vector<rs::core::TrackId> const& ids)
  {
    for (auto id : ids)
    {
      _allTrackIds->notifyUpdated(id);
      _rowDataProvider->invalidateHot(id);
    }
  }

  void MainWindow::notifyTracksRemoved(std::vector<rs::core::TrackId> const& ids)
  {
    for (auto id : ids)
    {
      _allTrackIds->notifyRemoved(id);
      _rowDataProvider->remove(id);
    }
  }

  void MainWindow::onTrackSelectionChanged()
  {
    auto* ctx = currentVisibleTrackPageContext();
    if (!ctx || !ctx->page)
    {
      if (_statusBar)
      {
        _statusBar->setSelectionInfo(0);
      }
      return;
    }

    auto const count = ctx->page->getSelectedTrackIds().size();
    auto const duration = ctx->page->getSelectedTracksDuration();

    if (_statusBar)
    {
      _statusBar->setSelectionInfo(count, duration);
    }
  }

  void MainWindow::updateImportProgress(double fraction, std::string const& info)
  {
    if (_statusBar)
    {
      if (fraction >= 1.0)
      {
        _statusBar->clearImportProgress();
        _statusBar->setTrackCount(_allTrackIds->size());
      }
      else
      {
        _statusBar->setImportProgress(fraction, info);
      }
    }
  }

  void MainWindow::saveSession()
  {
    try
    {
      auto windowState = _appConfig.windowState();

      if (auto const width = get_width(); width > 0)
      {
        windowState.width = width;
      }

      if (auto const height = get_height(); height > 0)
      {
        windowState.height = height;
      }

      if (auto const panedPosition = _paned.get_position(); panedPosition > 0)
      {
        windowState.panedPosition = panedPosition;
      }

      windowState.maximized = is_maximized();
      _appConfig.setWindowState(windowState);

      auto sessionState = _appConfig.sessionState();
      sessionState.lastLibraryPath = _musicLibrary ? normalizeLibraryPath(_musicLibrary->rootPath()) : std::string{};
      _appConfig.setSessionState(std::move(sessionState));
      if (_trackColumnLayoutModel)
      {
        _appConfig.setTrackViewState(trackViewStateFromLayout(_trackColumnLayoutModel->layout()));
      }
      _appConfig.save();
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to save app session: {}", e.what());
    }
  }

  void MainWindow::loadSession()
  {
    try
    {
      _appConfig = app::core::AppConfig::load();
      if (_trackColumnLayoutModel)
      {
        _trackColumnLayoutModel->setLayout(trackColumnLayoutFromState(_appConfig.trackViewState()));
      }

      auto const& windowState = _appConfig.windowState();
      set_default_size(windowState.width, windowState.height);

      if (windowState.panedPosition > 0)
      {
        _paned.set_position(windowState.panedPosition);
      }

      if (windowState.maximized)
      {
        maximize();
      }

      auto const& sessionState = _appConfig.sessionState();
      if (sessionState.lastLibraryPath.empty())
      {
        return;
      }

      auto const libraryPath = std::filesystem::path{sessionState.lastLibraryPath};
      if (std::filesystem::exists(libraryPath / "data.mdb"))
      {
        openMusicLibrary(libraryPath);
      }
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to load app session: {}", e.what());
    }
  }

  void MainWindow::setupPlayback()
  {
    _playbackBar = std::make_unique<app::ui::PlaybackBar>();
    _playbackController = std::make_unique<app::core::playback::PlaybackController>();

    _playbackBar->signalPlayRequested().connect(sigc::mem_fun(*this, &MainWindow::onPlayRequested));
    _playbackBar->signalPauseRequested().connect(sigc::mem_fun(*this, &MainWindow::onPauseRequested));
    _playbackBar->signalStopRequested().connect(sigc::mem_fun(*this, &MainWindow::onStopRequested));
    _playbackBar->signalSeekRequested().connect(sigc::mem_fun(*this, &MainWindow::onSeekRequested));

    // Start GTK timer to poll playback snapshot
    _playbackTimer = g_timeout_add(
      100,
      [](gpointer data) -> gboolean
      {
        static_cast<MainWindow*>(data)->refreshPlaybackBar();
        return G_SOURCE_CONTINUE;
      },
      this);
  }

  void MainWindow::refreshPlaybackBar()
  {
    if (!_playbackBar || !_playbackController)
    {
      return;
    }

    auto snapshot = _playbackController->snapshot();
    auto const previousState = _lastPlaybackState;
    _lastPlaybackState = snapshot.state;

    _playbackBar->setSnapshot(snapshot);

    if (_statusBar)
    {
      _statusBar->setPlaybackDetails(snapshot);
    }

    if (previousState == app::core::playback::TransportState::Playing &&
        snapshot.state == app::core::playback::TransportState::Idle)
    {
      handlePlaybackFinished();
    }
  }

  void MainWindow::onPlayRequested()
  {
    if (_playbackController)
    {
      auto const snapshot = _playbackController->snapshot();

      if (snapshot.state == app::core::playback::TransportState::Paused)
      {
        _playbackController->resume();
        return;
      }

      auto const* ctx = currentVisibleTrackPageContext();
      if (ctx && ctx->page)
      {
        if (auto const trackId = ctx->page->getPrimarySelectedTrackId(); trackId)
        {
          startPlaybackFromVisiblePage(*ctx->page, *trackId);
        }
      }
    }
  }

  void MainWindow::onPauseRequested()
  {
    if (_playbackController)
    {
      _playbackController->pause();
    }
  }

  void MainWindow::onStopRequested()
  {
    if (_playbackController)
    {
      clearActivePlaybackSequence();
      _lastPlaybackState = app::core::playback::TransportState::Idle;
      _playbackController->stop();
    }
  }

  void MainWindow::onSeekRequested(std::uint32_t positionMs)
  {
    if (_playbackController)
    {
      _playbackController->seek(positionMs);
    }
  }

  void MainWindow::playCurrentSelection()
  {
    if (_playbackController)
    {
      auto const snapshot = _playbackController->snapshot();

      if (snapshot.state == app::core::playback::TransportState::Paused)
      {
        _playbackController->resume();
        return;
      }

      auto const* ctx = currentVisibleTrackPageContext();
      if (ctx && ctx->page)
      {
        if (auto const trackId = ctx->page->getPrimarySelectedTrackId(); trackId)
        {
          startPlaybackFromVisiblePage(*ctx->page, *trackId);
        }
      }
    }
  }

  void MainWindow::pausePlayback()
  {
    if (_playbackController)
    {
      _playbackController->pause();
    }
  }

  void MainWindow::stopPlayback()
  {
    if (_playbackController)
    {
      clearActivePlaybackSequence();
      _lastPlaybackState = app::core::playback::TransportState::Idle;
      _playbackController->stop();
    }
  }

  void MainWindow::seekPlayback(std::uint32_t positionMs)
  {
    if (_playbackController)
    {
      _playbackController->seek(positionMs);
    }
  }

  bool MainWindow::startPlaybackFromVisiblePage(TrackViewPage const& page, rs::core::TrackId trackId)
  {
    return startPlaybackSequence(page.getVisibleTrackIds(), trackId);
  }

  bool MainWindow::startPlaybackSequence(std::vector<rs::core::TrackId> trackIds, rs::core::TrackId startTrackId)
  {
    if (!_playbackController || !_rowDataProvider)
    {
      return false;
    }

    auto const it = std::ranges::find(trackIds, startTrackId);
    auto startIndex = std::size_t{0};

    if (it == trackIds.end())
    {
      trackIds = {startTrackId};
    }
    else
    {
      startIndex = static_cast<std::size_t>(std::distance(trackIds.begin(), it));
    }

    ActivePlaybackSequence sequence;
    sequence.trackIds = std::move(trackIds);
    sequence.currentIndex = startIndex;
    _activePlaybackSequence = std::move(sequence);

    if (playTrackAtSequenceIndex(startIndex))
    {
      return true;
    }

    clearActivePlaybackSequence();
    return false;
  }

  bool MainWindow::playTrackAtSequenceIndex(std::size_t index)
  {
    if (!_playbackController || !_rowDataProvider || !_activePlaybackSequence)
    {
      return false;
    }

    auto& sequence = *_activePlaybackSequence;

    for (auto i = index; i < sequence.trackIds.size(); ++i)
    {
      if (auto descriptor = _rowDataProvider->getPlaybackDescriptor(sequence.trackIds[i]))
      {
        sequence.currentIndex = i;
        _playbackController->play(*descriptor);
        return true;
      }
    }

    return false;
  }

  void MainWindow::clearActivePlaybackSequence()
  {
    _activePlaybackSequence.reset();
  }

  void MainWindow::handlePlaybackFinished()
  {
    if (!_activePlaybackSequence)
    {
      return;
    }

    auto const nextIndex = _activePlaybackSequence->currentIndex + 1;
    if (playTrackAtSequenceIndex(nextIndex))
    {
      return;
    }

    clearActivePlaybackSequence();

    if (_playbackController)
    {
      _lastPlaybackState = app::core::playback::TransportState::Idle;
      _playbackController->stop();
    }
  }

  std::optional<app::core::playback::TrackPlaybackDescriptor> MainWindow::currentSelectionPlaybackDescriptor() const
  {
    auto const* ctx = currentVisibleTrackPageContext();

    if (!ctx || !ctx->page)
    {
      return std::nullopt;
    }

    // Get primary selected track
    auto trackId = ctx->page->getPrimarySelectedTrackId();

    if (!trackId)
    {
      return std::nullopt;
    }

    // Get playback descriptor
    return _rowDataProvider->getPlaybackDescriptor(*trackId);
  }

  void MainWindow::bindTrackPagePlayback(TrackViewPage& page)
  {
    page.signalTrackActivated().connect([this, pagePtr = &page](TrackListAdapter::TrackId trackId)
                                        { startPlaybackFromVisiblePage(*pagePtr, trackId); });
  }

  TrackPageContext* MainWindow::currentVisibleTrackPageContext()
  {
    auto* visibleChild = _stack.get_visible_child();

    if (!visibleChild)
    {
      return nullptr;
    }

    for (auto& [_, ctx] : _trackPages)
    {
      if (ctx.page.get() == visibleChild)
      {
        return &ctx;
      }
    }

    return nullptr;
  }

  TrackPageContext const* MainWindow::currentVisibleTrackPageContext() const
  {
    auto const* visibleChild = _stack.get_visible_child();

    if (!visibleChild)
    {
      return nullptr;
    }

    for (auto const& [_, ctx] : _trackPages)
    {
      if (ctx.page.get() == visibleChild)
      {
        return &ctx;
      }
    }

    return nullptr;
  }

} // namespace app::ui

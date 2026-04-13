// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "MainWindow.h"

#include <rs/core/ListBuilder.h>
#include <rs/core/MusicLibrary.h>
#include <rs/core/ResourceStore.h>
#include <rs/core/TrackBuilder.h>

#include "CoverArtWidget.h"
#include "ImportProgressDialog.h"
#include "ImportWorker.h"
#include "ListRow.h"
#include "NewListDialog.h"
#include "PlaybackBar.h"
#include "PlaylistExporter.h"
#include "TagPromptDialog.h"
#include "TrackListAdapter.h"
#include "TrackViewPage.h"
#include "model/AllTrackIdsList.h"
#include "model/FilteredTrackIdList.h"
#include "model/ListDraft.h"
#include "model/ManualTrackIdList.h"
#include "model/TrackIdList.h"
#include "model/TrackRowDataProvider.h"
#include "playback/PlaybackController.h"

#include <glibmm/keyfile.h>
#include <gtkmm/filedialog.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
  auto rootSourceListId() -> rs::core::ListId
  {
    return rs::core::ListId{0};
  }

  auto allTracksListId() -> rs::core::ListId
  {
    return rs::core::ListId{std::numeric_limits<std::uint32_t>::max()};
  }

  auto pageNameForListId(rs::core::ListId listId) -> std::string
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
    rs::core::ListId sourceListId = rootSourceListId();
    std::string name;
    bool isSmart = false;
    std::string localExpression;
  };
}

MainWindow::MainWindow()
  : _musicLibrary{nullptr}
  , _rowDataProvider{nullptr}
  , _allTrackIds{nullptr}
  , _coverArtWidget{nullptr}
  , _importDialog{nullptr}
  , _importWorker{nullptr}
  , _listStore{nullptr}
  , _listSelectionModel{nullptr}
  , _trackPages{}
  , _playbackBar{nullptr}
  , _playbackController{nullptr}
{
  set_title("RockStudio");

  // Set default window size
  constexpr int DefaultWindowWidth = 989;
  constexpr int DefaultWindowHeight = 801;
  set_default_size(DefaultWindowWidth, DefaultWindowHeight);

  // Initialize cover art widget
  _coverArtWidget = std::make_unique<CoverArtWidget>();

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

      if (folder)
      {
        std::filesystem::path path(folder->get_path());
        std::cout << "Selected folder: " << path << std::endl;

        // Check if it's an existing library (contains data.mdb) or a new import
        auto libPath = path / "data.mdb";
        std::cout << "DEBUG: libPath = " << libPath << std::endl;
        std::cout << "DEBUG: exists = " << std::filesystem::exists(libPath) << std::endl;

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
      std::cerr << "Error selecting folder: " << e.what() << std::endl; // NOLINT(bugprone-empty-catch)
    }
  });
}

void MainWindow::openMusicLibrary(std::filesystem::path const& path)
{
  std::cout << "DEBUG: openMusicLibrary called with path: " << path << std::endl;

  // Clear existing track pages first (adapters hold pointers to old _allTrackIds)
  clearTrackPages();

  // Create new music library at the path
  _musicLibrary = std::make_unique<rs::core::MusicLibrary>(path.string());
  std::cout << "DEBUG: MusicLibrary created" << std::endl;

  // Initialize row data provider with library stores
  _rowDataProvider = std::make_shared<app::model::TrackRowDataProvider>(*_musicLibrary);

  // Initialize AllTrackIdsList
  _allTrackIds = std::make_unique<app::model::AllTrackIdsList>(_musicLibrary->tracks());

  // Initialize SmartListEngine for smart lists
  _smartListEngine = std::make_unique<app::model::SmartListEngine>(*_musicLibrary);

  // Load all track IDs
  auto txn = _musicLibrary->readTransaction();
  _allTrackIds->reloadFromStore(txn);

  // Load existing lists
  rebuildListPages(txn);

  // Show the "All Tracks" page
  _stack.set_visible_child(pageNameForListId(allTracksListId()));

  // Update window title
  set_title("RockStudio [" + path.string() + "]");

  // Save session
  saveSession();
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
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
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
    std::cerr << "Error scanning directory: " << e.what() << std::endl; // NOLINT(bugprone-empty-catch)
  }
}

void MainWindow::importFiles()
{
  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Import Music Files");

  dialog->select_folder(*this,
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
      std::cout << "Importing from: " << pathStr << std::endl;

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
        std::cerr << "No music files found" << std::endl;
        return;
      }

      // Create progress dialog owned by MainWindow (stored as member)
      _importDialog = std::make_unique<ImportProgressDialog>(static_cast<int>(files.size()), *this);
      auto* dialogPtr = _importDialog.get();
      _importDialog->signal_response().connect([dialogPtr](int /*responseId*/) { dialogPtr->close(); });

      // Create worker - owned by MainWindow
      _importWorker = std::make_unique<ImportWorker>(*_musicLibrary,
                                                     files,
                                                     [dialogPtr](std::filesystem::path const& path, int index)
      {
        // Progress callback - marshal to main thread
        Glib::MainContext::get_default()->invoke([dialogPtr, path, index]()
        {
          if (dialogPtr)
          {
            dialogPtr->onNewTrack(path.string(), index);
          }

          return false;
        });
      },
                                                     [this, dialogPtr]()
      {
        // Finished callback - marshal to main thread
        Glib::MainContext::get_default()->invoke([this, dialogPtr]()
        {
          if (dialogPtr)
          {
            dialogPtr->ready();
          }

          return false;
        });
      });

      // Run in background thread - owned and joined on window destruction
      auto* workerPtr = _importWorker.get();
      if (_importThread.joinable())
      {
        _importThread.join();
      }
      _importThread = std::jthread([this, workerPtr](std::stop_token stoken)
      {
        workerPtr->run();
        // After import completes, notify observers incrementally
        Glib::MainContext::get_default()->invoke([this, workerPtr]()
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
      std::cerr << "Error selecting folder: " << e.what() << std::endl; // NOLINT(bugprone-empty-catch)
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
  _rowDataProvider = std::make_shared<app::model::TrackRowDataProvider>(*_musicLibrary);

  // Initialize AllTrackIdsList
  _allTrackIds = std::make_unique<app::model::AllTrackIdsList>(_musicLibrary->tracks());

  // Initialize SmartListEngine for smart lists
  _smartListEngine = std::make_unique<app::model::SmartListEngine>(*_musicLibrary);

  // Scan for music files
  std::vector<std::filesystem::path> files;
  scanDirectory(path, files);

  if (files.empty())
  {
    std::cerr << "No music files found" << std::endl;
    return;
  }

  // Show progress dialog
  _importDialog = std::make_unique<ImportProgressDialog>(static_cast<int>(files.size()), *this);
  auto* dialogPtr = _importDialog.get();
  _importDialog->signal_response().connect([dialogPtr](int /*responseId*/) { dialogPtr->close(); });

  // Create worker - owned by MainWindow
  _importWorker = std::make_unique<ImportWorker>(*_musicLibrary,
                                                 files,
                                                 [dialogPtr](std::filesystem::path const& path, int index)
  {
    Glib::MainContext::get_default()->invoke([dialogPtr, path, index]()
    {
      dialogPtr->onNewTrack(path.string(), index);
      return false;
    });
  },
                                                 [dialogPtr]()
  {
    Glib::MainContext::get_default()->invoke([dialogPtr]()
    {
      dialogPtr->ready();
      return false;
    });
  });

  // Run in background thread - owned and joined on window destruction
  auto* workerPtr = _importWorker.get();
  if (_importThread.joinable())
  {
    _importThread.join();
  }
  _importThread = std::jthread([this, workerPtr](std::stop_token stoken)
  {
    workerPtr->run();
    // After import completes, notify observers incrementally
    Glib::MainContext::get_default()->invoke([this, workerPtr]()
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
  app::model::TrackIdList* parentMembershipList = nullptr;
  if (parentListId == allTracksListId())
  {
    // Use All Tracks as source
    parentMembershipList = _allTrackIds.get();
  }
  else
  {
    // Find the parent's membership list from track pages
    auto const it = _trackPages.find(parentListId);
    if (it != _trackPages.end() && it->second.membershipList)
    {
      parentMembershipList = it->second.membershipList.get();
    }
    else
    {
      // Fallback to All Tracks if parent not found
      parentMembershipList = _allTrackIds.get();
    }
  }

  auto* dialog = Gtk::make_managed<NewListDialog>(*this, *_musicLibrary, *_allTrackIds, *parentMembershipList, parentListId);

  dialog->signal_response().connect([this, dialog](int responseId)
  {
    if (responseId == Gtk::ResponseType::OK)
    {
      createList(dialog->draft());
    }

    dialog->close();
  });

  dialog->present();
}

void MainWindow::openNewSmartListDialog()
{
  // Smart selection: if a non-All-Tracks list is selected, use it as parent; otherwise use root
  auto defaultSourceListId = rootSourceListId();
  auto const selected = _listSelectionModel ? _listSelectionModel->get_selected() : GTK_INVALID_LIST_POSITION;
  if (_listStore && selected != GTK_INVALID_LIST_POSITION && selected != 0)
  {
    if (auto row = _listStore->get_item(selected))
    {
      defaultSourceListId = row->getListId();
    }
  }

  openNewListDialog(defaultSourceListId);
}

bool MainWindow::listHasChildren(rs::core::ListId listId) const
{
  if (!_listStore)
  {
    return false;
  }

  auto const itemCount = _listStore->get_n_items();
  for (guint index = 1; index < itemCount; ++index)
  {
    auto row = _listStore->get_item(index);
    if (row && row->getSourceListId() == listId)
    {
      return true;
    }
  }

  return false;
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
  {
    openNewSmartListDialog();
  });
  _newListAction->set_enabled(false);
  add_action(_newListAction);

  _deleteListAction = Gio::SimpleAction::create("delete-list");
  _deleteListAction->signal_activate().connect([this]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
  { onDeleteList(); });
  _deleteListAction->set_enabled(false);
  add_action(_deleteListAction);
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

  // Create list store for sidebar
  _listStore = Gio::ListStore<ListRow>::create();

  // Create selection model
  _listSelectionModel = Gtk::SingleSelection::create(_listStore);
  _listSelectionModel->signal_selection_changed().connect(sigc::mem_fun(*this, &MainWindow::onListSelectionChanged));

  // List view for the sidebar
  auto factory = Gtk::SignalListItemFactory::create();
  factory->signal_setup().connect([this](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    rowBox->set_halign(Gtk::Align::FILL);
    rowBox->set_hexpand(true);
    rowBox->set_margin_start(6);
    rowBox->set_margin_end(6);
    rowBox->set_margin_top(3);
    rowBox->set_margin_bottom(3);

    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    label->set_hexpand(true);
    rowBox->append(*label);

    auto clickController = Gtk::GestureClick::create();
    clickController->set_button(GDK_BUTTON_SECONDARY);
    clickController->signal_pressed().connect([this, listItem, rowBox](int /*nPress*/, double x, double y)
    {
      auto const position = listItem->get_position();
      if (position != GTK_INVALID_LIST_POSITION)
      {
        _listSelectionModel->set_selected(position);
      }

      auto point = rowBox->compute_point(_listView, Gdk::Graphene::Point(static_cast<float>(x), static_cast<float>(y)));
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
  factory->signal_bind().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto item = listItem->get_item();
    auto row = std::dynamic_pointer_cast<ListRow>(item);
    auto box = dynamic_cast<Gtk::Box*>(listItem->get_child());
    auto label = box ? dynamic_cast<Gtk::Label*>(box->get_first_child()) : nullptr;
    if (row && label)
    {
      box->set_margin_start(6 + (std::max(row->getDepth(), 0) * 18));
      label->set_text(row->getName());
    }
  });

  _listView.set_factory(factory);
  _listView.set_model(_listSelectionModel);
  _listView.set_halign(Gtk::Align::FILL);
  _listView.set_valign(Gtk::Align::FILL);
  _listView.set_hexpand(true);
  _listView.set_vexpand(true);

  auto listContextMenuModel = Gio::Menu::create();
  listContextMenuModel->append("New List", "win.new-list");
  listContextMenuModel->append("Delete List", "win.delete-list");
  _listContextMenu.set_menu_model(listContextMenuModel);
  _listContextMenu.set_has_arrow(false);
  _listContextMenu.set_parent(_listView);

  // Scrolled window for list
  _listScrolledWindow.set_child(_listView);
  _listScrolledWindow.set_vexpand(true);

  // Cover art widget - matches Qt's CoverArtLabel (vsizetype=Maximum, min 50x50)
  _coverArtWidget->set_valign(Gtk::Align::END);
  _coverArtWidget->set_size_request(50, 50);
  _coverArtWidget->set_vexpand(false);

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
  constexpr int PanedInitialPosition = 330;
  _paned.set_position(PanedInitialPosition);

  // Set up the main layout
  auto* mainBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  mainBox->append(_menuBar);
  if (_playbackBar)
  {
    mainBox->append(*_playbackBar);
  }
  mainBox->append(_paned);

  // Set as window child
  set_child(*mainBox);
}

void MainWindow::showListContextMenu(Gtk::ListView& listView, Gdk::Rectangle const& rect)
{
  (void)listView;

  auto const hasLibrary = static_cast<bool>(_musicLibrary);

  auto canDelete = false;
  auto const selected = _listSelectionModel ? _listSelectionModel->get_selected() : GTK_INVALID_LIST_POSITION;
  if (hasLibrary && _listStore && selected != GTK_INVALID_LIST_POSITION && selected != 0)
  {
    if (auto row = _listStore->get_item(selected))
    {
      canDelete = !listHasChildren(row->getListId());
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

  _listContextMenu.set_pointing_to(rect);
  _listContextMenu.popup();
}

void MainWindow::createList(app::model::ListDraft const& draft)
{
  if (!_musicLibrary)
  {
    std::cerr << "No music library open" << std::endl;
    return;
  }

  auto txn = _musicLibrary->writeTransaction();

  // Build the list payload
  auto builder = rs::core::ListBuilder::createNew()
                   .name(draft.name)
                   .description(draft.description)
                   .sourceListId(draft.sourceListId);
  if (draft.kind == app::model::ListKind::Smart)
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

  auto const itemCount = _listStore->get_n_items();
  for (guint index = 0; index < itemCount; ++index)
  {
    auto row = _listStore->get_item(index);
    if (row && row->getListId() == listId)
    {
      _listSelectionModel->set_selected(index);
      break;
    }
  }
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

  auto item = _listStore->get_item(position);

  if (!item)
  {
    return;
  }

  auto listId = item->getListId();

  if (listHasChildren(listId))
  {
    std::cerr << "Cannot delete a list that still has child lists" << std::endl;
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
  std::cout << "DEBUG: rebuildListPages called" << std::endl;
  // Clear existing list store and track pages
  _listStore->remove_all();
  clearTrackPages();

  // Build "All Tracks" page
  buildPageForAllTracks();

  // Add "All Tracks" entry
  auto allRow = ListRow::create(allTracksListId(), rootSourceListId(), 0, false, "All Tracks");
  _listStore->append(allRow);

  auto reader = _musicLibrary->lists().reader(txn);
  auto nodes = std::map<rs::core::ListId, StoredListNode>{};
  for (auto const& [id, listView] : reader)
  {
    nodes.emplace(id,
                  StoredListNode{
                    .id = id,
                    .sourceListId = listView.sourceListId(),
                    .name = std::string(listView.name()),
                    .isSmart = listView.isSmart(),
                    .localExpression = std::string(listView.filter()),
                  });
  }

  auto children = std::map<rs::core::ListId, std::vector<rs::core::ListId>>{};
  auto roots = std::vector<rs::core::ListId>{};

  for (auto const& [id, node] : nodes)
  {
    if (node.sourceListId != rootSourceListId() && node.sourceListId != id && nodes.contains(node.sourceListId))
    {
      children[node.sourceListId].push_back(id);
    }
    else
    {
      roots.push_back(id);
    }
  }

  auto visitState = std::map<rs::core::ListId, int>{};
  auto orderedLists = std::vector<std::pair<rs::core::ListId, int>>{};

  std::function<void(rs::core::ListId, int)> appendPreorder = [&](rs::core::ListId id, int depth)
  {
    auto& state = visitState[id];
    if (state == 1 || state == 2)
    {
      return;
    }

    state = 1;
    orderedLists.emplace_back(id, depth);

    if (auto const childIt = children.find(id); childIt != children.end())
    {
      for (auto childId : childIt->second)
      {
        appendPreorder(childId, depth + 1);
      }
    }

    state = 2;
  };

  for (auto rootId : roots)
  {
    appendPreorder(rootId, 0);
  }

  for (auto const& [id, node] : nodes)
  {
    (void)node;
    if (!visitState.contains(id))
    {
      appendPreorder(id, 0);
    }
  }

  for (auto const& [id, depth] : orderedLists)
  {
    auto optView = reader.get(id);
    if (!optView)
    {
      continue;
    }

    auto const& listView = *optView;
    auto listRow = ListRow::create(id, listView.sourceListId(), depth, listView.isSmart(), std::string(listView.name()));
    _listStore->append(listRow);

    // Create track view page for this list
    buildPageForStoredList(id, listView);
  }

  // Set up track context menu for all track pages
  setupTrackContextMenu();
}

void MainWindow::buildPageForAllTracks()
{
  // Create adapter using the shared _allTrackIds (not a page-local copy)
  // _allTrackIds is the authoritative source that import/tag notifies update
  auto adapter = std::make_shared<TrackListAdapter>(*_allTrackIds, _rowDataProvider);
  // Manually trigger rebuild since notifyReset was called before adapter was attached
  adapter->onReset();
  auto trackPage = std::make_unique<TrackViewPage>(adapter);

  auto pageId = pageNameForListId(allTracksListId());
  _stack.add(*trackPage, pageId, "All Tracks");

  // Connect selection to cover art update
  trackPage->signalSelectionChanged().connect([this, trackPagePtr = trackPage.get()]()
  {
    auto ids = trackPagePtr->getSelectedTrackIds();
    updateCoverArt(ids);
  });

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
  auto const name = view.name();
  if (!name.empty())
  {
    listName = std::string(name);
  }
  else
  {
    listName = "<Unnamed List>";
  }

  // Create appropriate membership list based on smart vs manual
  std::unique_ptr<app::model::TrackIdList> membershipList;

  if (view.isSmart())
  {
    auto* sourceList = static_cast<app::model::TrackIdList*>(_allTrackIds.get());
    if (!view.isRootSource())
    {
      auto const sourceIt = _trackPages.find(view.sourceListId());
      if (sourceIt != _trackPages.end() && sourceIt->second.membershipList)
      {
        sourceList = sourceIt->second.membershipList.get();
      }
      else
      {
        std::cerr << "Missing source list for smart list " << listId << ", falling back to All Tracks" << std::endl;
      }
    }

    // Smart list - use FilteredTrackIdList
    auto filtered = std::make_unique<app::model::FilteredTrackIdList>(*sourceList, *_musicLibrary, *_smartListEngine);

    // Set expression from filter stored in payload
    auto expr = view.filter();
    filtered->setExpression(std::string(expr));
    filtered->reload();
    membershipList = std::move(filtered);
  }
  else
  {
    // Manual list - use ManualTrackIdList, observes _allTrackIds for updates/removes
    auto manual = std::make_unique<app::model::ManualTrackIdList>(view, _allTrackIds.get());
    membershipList = std::move(manual);
  }

  // Create adapter
  auto adapter = std::make_shared<TrackListAdapter>(*membershipList, _rowDataProvider);
  // Prime the model with the membership list contents because the list was populated
  // before the adapter attached as an observer.
  adapter->onReset();

  // Create track page
  auto trackPage = std::make_unique<TrackViewPage>(adapter);

  auto pageId = pageNameForListId(listId);
  _stack.add(*trackPage, pageId, listName);

  // Connect selection to cover art update
  trackPage->signalSelectionChanged().connect([this, trackPagePtr = trackPage.get()]()
  {
    auto ids = trackPagePtr->getSelectedTrackIds();
    updateCoverArt(ids);
  });

  // Connect track activation to playback
  bindTrackPagePlayback(*trackPage);

  // Create playlist exporter for this list
  auto playlistDir = _musicLibrary->rootPath() / "playlist";
  if (!std::filesystem::exists(playlistDir))
  {
    std::filesystem::create_directories(playlistDir);
  }
  auto playlistPath = playlistDir / (listName + ".m3u");
  auto exporter =
    std::make_unique<PlaylistExporter>(*membershipList, *_rowDataProvider, _musicLibrary->rootPath(), playlistPath);

  TrackPageContext ctx;
  ctx.membershipList = std::move(membershipList);
  ctx.adapter = std::move(adapter);
  ctx.page = std::move(trackPage);
  ctx.exporter = std::move(exporter);
  _trackPages[listId] = std::move(ctx);
}

void MainWindow::setupTrackContextMenu()
{
  // Create track tag action
  auto tagTrackAction = Gio::SimpleAction::create("tag-track");
  tagTrackAction->signal_activate().connect([this]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
  { onTagTrack(); });
  add_action(tagTrackAction);
}

void MainWindow::onTagTrack()
{
  auto* ctx = currentVisibleTrackPageContext();
  if (!ctx)
  {
    return;
  }

  auto selectedIds = ctx->page->getSelectedTrackIds();

  if (selectedIds.empty())
  {
    return;
  }

  // Show tag prompt dialog
  auto* dialog = Gtk::make_managed<TagPromptDialog>(*this);
  auto idsCopy = selectedIds; // Copy for lambda

  dialog->signal_response().connect([this, dialog, idsCopy](int responseId) mutable
  {
    if (responseId == Gtk::ResponseType::OK)
    {
      std::string tag = dialog->tag();
      if (!tag.empty())
      {
        // Open write transaction
        auto txn = _musicLibrary->writeTransaction();
        auto writer = _musicLibrary->tracks().writer(txn);
        auto& dict = _musicLibrary->dictionary();

        // Resolve tag string to DictionaryId
        auto tagId = dict.put(txn, tag);

        // Update each selected track
        for (auto trackId : idsCopy)
        {
          // Get current track data
          auto optView = writer.get(trackId, rs::core::TrackStore::Reader::LoadMode::Hot);
          if (!optView)
          {
            continue;
          }

          // Build TrackBuilder from current view
          auto builder = rs::core::TrackBuilder::fromView(*optView, dict);

          // Add new tag by resolving ID to name
          builder.tags().add(dict.get(tagId));

          // Zero-copy update hot data
          auto prepared = builder.prepareHot(txn, dict);
          writer.updateHot(trackId, prepared.size(), [&prepared](std::span<std::byte> hot) { prepared.writeTo(hot); });
        }

        // Commit transaction
        txn.commit();

        // After commit: invalidate cache and notify observers incrementally
        for (auto trackId : idsCopy)
        {
          _rowDataProvider->invalidateHot(trackId);
          _allTrackIds->notifyUpdated(trackId);
        }
      }
    }

    dialog->close();
  });
  dialog->present();
}

void MainWindow::onListSelectionChanged([[maybe_unused]] std::uint32_t position, [[maybe_unused]] std::uint32_t nItems)
{
  auto const selected = _listSelectionModel ? _listSelectionModel->get_selected() : GTK_INVALID_LIST_POSITION;
  if (selected == GTK_INVALID_LIST_POSITION)
  {
    return;
  }

  auto item = _listStore->get_item(selected);

  if (!item)
  {
    return;
  }

  auto listId = item->getListId();

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

void MainWindow::saveSession()
{
  // Placeholder - session saving would restore library path
}

void MainWindow::loadSession()
{
  // Placeholder - would restore previous library
}

void MainWindow::setupPlayback()
{
  _playbackBar = std::make_unique<PlaybackBar>();
  _playbackController = std::make_unique<app::playback::PlaybackController>();

  _playbackBar->signalPlayRequested().connect(sigc::mem_fun(*this, &MainWindow::onPlayRequested));
  _playbackBar->signalPauseRequested().connect(sigc::mem_fun(*this, &MainWindow::onPauseRequested));
  _playbackBar->signalStopRequested().connect(sigc::mem_fun(*this, &MainWindow::onStopRequested));
  _playbackBar->signalSeekRequested().connect(sigc::mem_fun(*this, &MainWindow::onSeekRequested));

  // Start GTK timer to poll playback snapshot
  _playbackTimer = g_timeout_add(100,
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
  _playbackBar->setSnapshot(snapshot);
}

void MainWindow::onPlayRequested()
{
  if (_playbackController)
  {
    auto descriptor = currentSelectionPlaybackDescriptor();
    if (descriptor)
    {
      _playbackController->play(*descriptor);
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
    auto descriptor = currentSelectionPlaybackDescriptor();
    if (descriptor)
    {
      _playbackController->play(*descriptor);
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

std::optional<app::playback::TrackPlaybackDescriptor> MainWindow::currentSelectionPlaybackDescriptor() const
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
  page.signalTrackActivated().connect([this](TrackListAdapter::TrackId trackId)
  {
    if (auto descriptor = _rowDataProvider->getPlaybackDescriptor(trackId))
    {
      _playbackController->play(*descriptor);
    }
  });
}

TrackPageContext* MainWindow::currentVisibleTrackPageContext()
{
  auto* visibleChild = _stack.get_visible_child();
  if (!visibleChild)
  {
    return nullptr;
  }

  for (auto& [id, ctx] : _trackPages)
  {
    (void)id;
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

  for (auto const& [id, ctx] : _trackPages)
  {
    (void)id;
    if (ctx.page.get() == visibleChild)
    {
      return &ctx;
    }
  }

  return nullptr;
}

#include "MainWindow.h"

#include <rs/core/MusicLibrary.h>

#include <flatbuffers/flatbuffers.h>

#include <glibmm/keyfile.h>
#include <gtkmm/filedialog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

#include "CoverArtWidget.h"
#include "ImportProgressDialog.h"
#include "ImportWorker.h"
#include "ListRow.h"
#include "NewListDialog.h"
#include "PlaylistExporter.h"
#include "TagPromptDialog.h"
#include "TrackListAdapter.h"
#include "TrackViewPage.h"

MainWindow::MainWindow()
{
  set_title("RockStudio");

  // Set default window size
  set_default_size(989, 801);

  // Initialize cover art widget
  _coverArtWidget = std::make_unique<CoverArtWidget>();

  setupMenu();
  setupLayout();

  // Try to restore previous session
  loadSession();
}

MainWindow::~MainWindow() = default;

void MainWindow::openLibrary()
{
  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Open Music Library");

  // Show the dialog
  dialog->select_folder(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
    try
    {
      auto folder = dialog->select_folder_finish(result);

      if (folder)
      {
        std::filesystem::path path(folder->get_path());
        std::cout << "Selected folder: " << path << std::endl;

        // Check if it's an existing library (contains data.mdb) or a new import
        auto libPath = path / "data.mdb";

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
    catch (const Glib::Error& e)
    {
      std::cerr << "Error selecting folder: " << e.what() << std::endl;
    }
  });
}

void MainWindow::openMusicLibrary(const std::filesystem::path& path)
{
  // Create new music library at the path
  _musicLibrary = std::make_unique<rs::core::MusicLibrary>(path.string());

  // Load all tracks into _allTracks
  auto txn = _musicLibrary->readTransaction();
  loadAllTracks(txn);

  // Load existing lists
  loadLists(txn);

  // Show the "All Tracks" page
  _stack.set_visible_child("0");

  // Update window title
  set_title("RockStudio [" + path.string() + "]");

  // Save session
  saveSession();
}

void MainWindow::scanDirectory(const std::filesystem::path& dir, std::vector<std::filesystem::path>& files)
{
  static const std::unordered_set<std::string> supportedExtensions = {".mp3", ".m4a", ".flac"};

  try
  {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
    {
      if (entry.is_regular_file())
      {
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (supportedExtensions.count(ext) > 0)
        {
          files.push_back(entry.path());
        }
      }
    }
  }
  catch (const std::exception& e)
  {
    std::cerr << "Error scanning directory: " << e.what() << std::endl;
  }
}

void MainWindow::importFiles()
{
  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Import Music Files");

  dialog->select_folder(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
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

      // Create worker - use shared_ptr to safely share with thread
      auto worker = std::make_unique<ImportWorker>(
        *_musicLibrary,
        files,
        [dialogPtr](const std::filesystem::path& path, int index) {
          // Progress callback - marshal to main thread
          Glib::MainContext::get_default()->invoke([dialogPtr, path, index]() {
            if (dialogPtr)
            {
              dialogPtr->onNewTrack(path.string(), index);
            }

            return false;
          });
        },
        [this, dialogPtr]() {
          // Finished callback - marshal to main thread
          Glib::MainContext::get_default()->invoke([this, dialogPtr]() {
            if (dialogPtr)
            {
              dialogPtr->ready();
            }

            return false;
          });
        });

      // Run in background thread using shared_ptr for safety
      auto workerPtr = worker.get();
      std::thread workerThread([workerPtr]() {
        workerPtr->run();
        workerPtr->commit();
      });
      workerThread.detach();

      _importDialog->show();
    }
    catch (const Glib::Error& e)
    {
      std::cerr << "Error selecting folder: " << e.what() << std::endl;
    }
  });
}

void MainWindow::importFilesFromPath(const std::filesystem::path& path)
{
  // Create new music library at the path
  _musicLibrary = std::make_unique<rs::core::MusicLibrary>(path.string());

  // Scan for music files
  std::vector<std::filesystem::path> files;
  scanDirectory(path, files);

  if (files.empty())
  {
    std::cerr << "No music files found" << std::endl;
    return;
  }

  // Show progress dialog
  auto progressDialog = std::make_unique<ImportProgressDialog>(static_cast<int>(files.size()), *this);

  // Create worker with callbacks
  ImportWorker* workerRaw = nullptr;
  auto worker = std::make_unique<ImportWorker>(
    *_musicLibrary,
    files,
    [&progressDialog](const std::filesystem::path& path, int index) {
      Glib::MainContext::get_default()->invoke([&progressDialog, path, index]() {
        progressDialog->onNewTrack(path.string(), index);
        return false;
      });
    },
    [&progressDialog]() {
      Glib::MainContext::get_default()->invoke([&progressDialog]() {
        progressDialog->ready();
        return false;
      });
    });
  workerRaw = worker.get();

  std::thread workerThread([workerRaw, this]() {
    workerRaw->run();
    workerRaw->commit();
    // Reload tracks after import
    Glib::MainContext::get_default()->invoke([this]() {
      _allTracks.clear();
      auto txn = _musicLibrary->readTransaction();
      loadAllTracks(txn);
      loadLists(txn);
      return false;
    });
  });
  workerThread.detach();

  progressDialog->show();
}

void MainWindow::setupMenu()
{
  // Create menu model
  auto menuModel = Gio::Menu::create();

  // File menu
  auto fileMenu = Gio::Menu::create();
  fileMenu->append("Open Library", "win.open-library");
  fileMenu->append("Import Files", "win.import-files");
  auto section = Gio::Menu::create();
  section->append("New List", "win.new-list");
  fileMenu->append_section(section);
  menuModel->append_submenu("File", fileMenu);

  // Help menu (placeholder)
  auto helpMenu = Gio::Menu::create();
  helpMenu->append("About", "app.about");
  menuModel->append_submenu("Help", helpMenu);

  // Set up menu bar
  _menuBar.set_menu_model(menuModel);

  // Create actions
  auto openAction = Gio::SimpleAction::create("open-library");
  openAction->signal_activate().connect([this]([[maybe_unused]] const Glib::VariantBase& v) { openLibrary(); });
  add_action(openAction);

  auto importAction = Gio::SimpleAction::create("import-files");
  importAction->signal_activate().connect([this]([[maybe_unused]] const Glib::VariantBase& v) { importFiles(); });
  add_action(importAction);

  auto newListAction = Gio::SimpleAction::create("new-list");
  newListAction->signal_activate().connect([this](const Glib::VariantBase& variant) {
    [[maybe_unused]] auto variantCopy = variant;
    // Create dialog - GTK4 will manage its lifetime when closed
    auto* dialog = Gtk::make_managed<NewListDialog>(*this);

    dialog->signal_response().connect([this, dialog](int responseId) {
      if (responseId == Gtk::ResponseType::OK)
      {
        auto list = dialog->list();
        createList(list);
      }
    });

    dialog->present();
  });
  add_action(newListAction);
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
  factory->signal_setup().connect([](const Glib::RefPtr<Gtk::ListItem>& listItem) {
    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    listItem->set_child(*label);
  });
  factory->signal_bind().connect([](const Glib::RefPtr<Gtk::ListItem>& listItem) {
    auto item = listItem->get_item();
    auto row = std::dynamic_pointer_cast<ListRow>(item);
    auto label = dynamic_cast<Gtk::Label*>(listItem->get_child());
    if (row && label)
    {
      label->set_text(row->getName());
    }
  });

  _listView.set_factory(factory);
  _listView.set_model(_listSelectionModel);
  _listView.set_halign(Gtk::Align::FILL);
  _listView.set_valign(Gtk::Align::FILL);
  _listView.set_hexpand(true);
  _listView.set_vexpand(true);

  // Note: Context menus in GTK4 are handled via gesture events
  // For now, we'll skip the popover implementation and rely on menu actions

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
  _paned.set_position(330);

  // Set up the main layout
  auto* mainBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  mainBox->append(_menuBar);
  mainBox->append(_paned);

  // Set as window child
  set_child(*mainBox);
}

void MainWindow::createList(const rs::fbs::ListT& list)
{
  if (!_musicLibrary)
  {
    std::cerr << "No music library open" << std::endl;
    return;
  }

  // Create the list in the library
  auto txn = _musicLibrary->writeTransaction();
  auto listId = _musicLibrary->lists().writer(txn).create([&list](flatbuffers::FlatBufferBuilder& fbb) {
    auto nameOffset = fbb.CreateString(list.name);
    auto descOffset = fbb.CreateString(list.desc);
    auto exprOffset = fbb.CreateString(list.expr);
    rs::fbs::ListBuilder builder{fbb};
    builder.add_name(nameOffset);
    builder.add_desc(descOffset);
    builder.add_expr(exprOffset);
    return builder.Finish();
  });
  [[maybe_unused]] auto listIdCopy = listId;
  txn.commit();

  // Refresh lists
  auto readTxn = _musicLibrary->readTransaction();
  loadLists(readTxn);
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

  // Delete the list from the library
  auto txn = _musicLibrary->writeTransaction();
  _musicLibrary->lists().writer(txn).del(listId);
  txn.commit();

  // Refresh lists
  auto readTxn = _musicLibrary->readTransaction();
  loadLists(readTxn);
}

void MainWindow::loadAllTracks(rs::lmdb::ReadTransaction& txn)
{
  for (auto [id, track] : _musicLibrary->tracks().reader(txn))
  {
    rs::fbs::TrackT tt;
    track->UnPackTo(&tt);
    _allTracks.insert(id, std::move(tt));
  }
}

void MainWindow::loadLists(rs::lmdb::ReadTransaction& txn)
{
  // Clear existing list store and track pages
  _listStore->remove_all();
  _trackPages.clear();

  using ListId = rs::core::MusicLibrary::ListId;

  // Create track view for "All Tracks" (list_id = 0)
  {
    auto adapter = std::make_shared<TrackListAdapter>(_allTracks);
    auto trackPage = std::make_unique<TrackViewPage>(adapter);
    auto pageId = std::to_string(static_cast<unsigned long>(ListId(0)));
    _stack.add(*trackPage, pageId, "All Tracks");

    // Connect selection to cover art update
    trackPage->signalSelectionChanged().connect([this, trackPagePtr = trackPage.get()]() {
      auto ids = trackPagePtr->getSelectedTrackIds();
      updateCoverArt(ids);
    });

    _trackPages[ListId(0)] = std::move(trackPage);
  }

  // Add "All Tracks" entry
  auto allRow = ListRow::create(ListId(0), "All Tracks");
  _listStore->append(allRow);

  // Load existing lists from library
  for (const auto [id, list] : _musicLibrary->lists().reader(txn))
  {
    rs::fbs::ListT lt;
    list->UnPackTo(&lt);

    auto listRow = ListRow::create(id, lt.name);
    _listStore->append(listRow);

    // Create track view page for this list with filtered tracks
    // Use expression from the list definition
    auto adapter = std::make_shared<TrackListAdapter>(_allTracks);
    if (!lt.expr.empty())
    {
      adapter->setExprFilter(lt.expr);
    }
    auto trackPage = std::make_unique<TrackViewPage>(adapter);
    auto pageId = std::to_string(static_cast<unsigned long>(id));
    _stack.add(*trackPage, pageId, lt.name);

    // Connect selection to cover art update
    trackPage->signalSelectionChanged().connect([this, trackPagePtr = trackPage.get()]() {
      auto ids = trackPagePtr->getSelectedTrackIds();
      updateCoverArt(ids);
    });

    // Create playlist exporter for this list
    auto playlistDir = _musicLibrary->rootPath() / "playlist";
    if (!std::filesystem::exists(playlistDir))
    {
      std::filesystem::create_directories(playlistDir);
    }
    auto playlistPath = playlistDir / (lt.name + ".m3u");
    auto exporter = std::make_unique<PlaylistExporter>(_allTracks, _musicLibrary->rootPath(), playlistPath);
    _playlistExporters[id] = std::move(exporter);

    _trackPages[id] = std::move(trackPage);
  }

  // Set up track context menu for all track pages
  setupTrackContextMenu();
}

void MainWindow::setupTrackContextMenu()
{
  // Create track tag action
  auto tagTrackAction = Gio::SimpleAction::create("tag-track");
  tagTrackAction->signal_activate().connect([this](const Glib::VariantBase& v) {
    [[maybe_unused]] auto vCopy = v;
    onTagTrack();
  });
  add_action(tagTrackAction);
}

void MainWindow::onTagTrack()
{
  // Get current visible track page
  Glib::ustring pageName = _stack.get_visible_child_name();
  if (pageName.empty())
    return;

  auto pageId = std::stoi(pageName);
  auto it = _trackPages.find(rs::core::MusicLibrary::ListId(pageId));
  if (it == _trackPages.end())
  {
    return;
  }

  auto& trackPage = it->second;
  auto selectedIds = trackPage->getSelectedTrackIds();

  if (selectedIds.empty())
  {
    return;
  }

  // Show tag prompt dialog
  auto* dialog = Gtk::make_managed<TagPromptDialog>(*this);
  auto idsCopy = selectedIds; // Copy for lambda

  dialog->signal_response().connect([this, dialog, idsCopy](int responseId) mutable {
    if (responseId == Gtk::ResponseType::OK)
    {
      Glib::ustring tag = dialog->tag();
      if (!tag.empty())
      {
        // Add tag to all selected tracks
        auto txn = _musicLibrary->writeTransaction();
        auto writer = _musicLibrary->tracks().writer(txn);

        for (auto trackId : idsCopy)
        {
          _allTracks.update(trackId, [tag, &writer, trackId](rs::fbs::TrackT& track) {
            track.tags.push_back(tag);
            writer.updateT(trackId, track);
          });
        }

        txn.commit();
      }
    }
  });
  dialog->present();
}

void MainWindow::onListSelectionChanged(std::uint32_t position, [[maybe_unused]] std::uint32_t nItems)
{
  if (position == GTK_INVALID_LIST_POSITION)
  {
    return;
  }

  auto item = _listStore->get_item(position);

  if (!item)
  {
    return;
  }

  auto listId = item->getListId();

  // Switch to the corresponding stack page
  auto pageId = std::to_string(static_cast<unsigned long>(listId));
  _stack.set_visible_child(pageId);
}

void MainWindow::updateCoverArt(const std::vector<rs::core::MusicLibrary::TrackId>& selectedIds)
{
  if (!_musicLibrary || selectedIds.empty())
  {
    // Clear cover art - use default
    _coverArtWidget->clearCover();
    return;
  }

  // Get the first selected track
  auto trackId = selectedIds.front();

  // Look up the track in _allTracks to get resource ID
  auto* ttPtr = _allTracks.find(trackId);
  if (ttPtr)
  {
    auto& tt = *ttPtr;

    // Check if track has resources (album art)
    if (!tt.rsrc.empty())
    {
      // Get the first resource (album art)
      const auto& rsrc = tt.rsrc[0];
      if (rsrc->type == rs::fbs::ResourceType::AlbumArt)
      {
        // Read the resource data
        auto resourceTxn = _musicLibrary->readTransaction();
        auto resourceReader = _musicLibrary->resources().reader(resourceTxn);
        auto data = resourceReader[rsrc->id];
        auto size = boost::asio::buffer_size(data);

        if (size > 0)
        {
          // Load image from memory - write to temp file first
          try
          {
            const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data.data());
            // Write to temp file
            std::filesystem::path tempPath = std::filesystem::temp_directory_path() / "rockstudio_coverart.jpg";
            std::ofstream ofs(tempPath, std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(bytes), size);
            ofs.close();

            // Load from file
            auto pixbuf = Gdk::Pixbuf::create_from_file(tempPath.string());
            _coverArtWidget->setCoverPixbuf(pixbuf);

            // Clean up temp file
            std::filesystem::remove(tempPath);
            return;
          }
          catch (const Glib::Error& e)
          {
            std::cerr << "Failed to load cover art: " << e.what() << std::endl;
          }
        }
      }
    }
  }

  // No cover art found - clear
  _coverArtWidget->clearCover();
}

void MainWindow::saveSession()
{
  if (!_musicLibrary)
    return;

  try
  {
    auto keyfile = Glib::KeyFile::create();
    keyfile->set_string("session", "lastLibraryPath", std::string(_musicLibrary->rootPath()));

    // Save to user config directory
    auto configDir = Glib::get_user_config_dir();
    auto configPath = std::filesystem::path(configDir) / "rockstudio" / "session.ini";

    // Create directory if needed
    std::filesystem::create_directories(configPath.parent_path());

    keyfile->save_to_file(configPath.string());
  }
  catch (const Glib::Error& e)
  {
    std::cerr << "Failed to save session: " << e.what() << std::endl;
  }
}

void MainWindow::loadSession()
{
  try
  {
    auto configDir = Glib::get_user_config_dir();
    auto configPath = std::filesystem::path(configDir) / "rockstudio" / "session.ini";

    if (!std::filesystem::exists(configPath))
      return;

    auto keyfile = Glib::KeyFile::create();
    keyfile->load_from_file(configPath.string());

    auto lastPath = keyfile->get_string("session", "lastLibraryPath");
    if (lastPath.empty())
      return;

    // Check if path exists
    std::filesystem::path libPath(lastPath);
    if (std::filesystem::exists(libPath))
    {
      auto dataPath = libPath / "data.mdb";
      if (std::filesystem::exists(dataPath))
      {
        openMusicLibrary(libPath);
      }
    }
  }
  catch (const Glib::Error& e)
  {
    std::cerr << "Failed to load session: " << e.what() << std::endl;
  }
}

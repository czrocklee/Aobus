// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/MainWindow.h"
#include "platform/linux/services/PlaylistExporter.h"
#include <rs/audio/PlaybackController.h>
#include <rs/utility/Log.h>

#include "platform/linux/ui/CoverArtWidget.h"
#include "platform/linux/ui/ImportProgressDialog.h"
#include "platform/linux/ui/LayoutConstants.h"
#include "platform/linux/ui/ListRow.h"
#include "platform/linux/ui/ListTreeNode.h"
#include "platform/linux/ui/PlaybackBar.h"
#include "platform/linux/ui/SmartListDialog.h"
#include "platform/linux/ui/TagPopover.h"
#include "platform/linux/ui/TrackListAdapter.h"
#include "platform/linux/ui/TrackRowDataProvider.h"
#include "platform/linux/ui/TrackViewPage.h"

#include <glibmm/keyfile.h>
#include <glibmm/variant.h>
#include <gtkmm/filedialog.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <ranges>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

namespace app::ui
{
  namespace
  {
    rs::ListId allTracksListId()
    {
      return rs::ListId{std::numeric_limits<std::uint32_t>::max()};
    }

    rs::ListId rootParentId()
    {
      return rs::ListId{0};
    }
  }

  MainWindow::MainWindow()
    : _librarySession{nullptr}, _coverArtWidget{nullptr}
  {
    set_title("RockStudio");

    // Set default window size
    set_default_size(rs::library::kDefaultWindowWidth, rs::library::kDefaultWindowHeight);

    // Initialize cover art widget
    _coverArtWidget = std::make_unique<CoverArtWidget>();

    // Initialize dispatcher
    _dispatcher = std::make_shared<GtkMainThreadDispatcher>();

    // Initialize TagEditController
    _tagEditController = std::make_unique<TagEditController>(
      *this,
      TagEditController::Callbacks{.onStatusMessage = [this](std::string const& msg) { showStatusMessage(msg); },
                                   .onTagsMutated =
                                     []()
                                   {
                                     // Additional invalidation/update logic if needed
                                   }});

    // Initialize track page graph
    _trackPageGraph = std::make_unique<TrackPageGraph>(
      _stack,
      _trackColumnLayoutModel,
      TrackPageGraph::Callbacks{
        .onSelectionChanged =
          [this](std::vector<rs::TrackId> const& ids)
        {
          updateCoverArt(ids);
          onTrackSelectionChanged();
        },
        .onContextMenuRequested =
          [this](TrackViewPage& page, double posX, double posY)
        {
          auto const listId = page.getListId();
          auto* const ctx = _trackPageGraph->find(listId);
          TrackSelectionContext const selection{.listId = listId,
                                                .selectedIds = page.getSelectedTrackIds(),
                                                .membershipList = ctx != nullptr ? ctx->membershipList.get() : nullptr};
          _tagEditController->showTrackContextMenu(page, selection, posX, posY);
        },
        .onTagEditRequested =
          [this](TrackViewPage& page, std::vector<rs::TrackId> const& ids, double posX, double posY)
        {
          auto const listId = page.getListId();
          auto* const ctx = _trackPageGraph->find(listId);
          TrackSelectionContext const selection{.listId = listId,
                                                .selectedIds = ids,
                                                .membershipList = ctx != nullptr ? ctx->membershipList.get() : nullptr};
          _tagEditController->showTagEditor(page, selection, posX, posY);
        },
        .onTrackActivated = [this](TrackViewPage& page, rs::TrackId id)
        { _playbackCoordinator->startPlaybackFromVisiblePage(page, id); },
        .onCreateSmartListRequested =
          [this](TrackViewPage& page, std::string const& expression)
        {
          if (_listSidebarController == nullptr)
          {
            return;
          }

          auto parentListId = page.getListId();

          if (parentListId == allTracksListId())
          {
            parentListId = rootParentId();
          }

          _listSidebarController->createSmartListFromExpression(parentListId, expression);
        }});

    // Initialize playback coordinator
    _playbackCoordinator = std::make_unique<PlaybackCoordinator>(
      *this, _dispatcher, [this]() { return _librarySession ? _librarySession->rowDataProvider.get() : nullptr; });

    // Initialize import/export coordinator
    _importExportCoordinator = std::make_unique<ImportExportCoordinator>(
      *this,
      ImportExportCallbacks{
        .getCurrentSession = [this]() { return _librarySession.get(); },
        .onLibrarySessionCreated = [this](std::unique_ptr<LibrarySession> session)
        { installLibrarySession(std::move(session)); },
        .onLibraryDataMutated =
          [this]()
        {
          if (_librarySession)
          {
            _librarySession->rowDataProvider->loadAll();

            auto txn = _librarySession->musicLibrary->readTransaction();
            _librarySession->allTrackIds->reloadFromStore(txn);
            rebuildListPages(txn);

            if (_statusBar)
            {
              _statusBar->setTrackCount(_librarySession->allTrackIds->size());
            }
          }
        },
        .onProgressUpdated = [this](double fraction, std::string const& info) { updateImportProgress(fraction, info); },
        .onStatusMessage = [this](std::string const& msg) { showStatusMessage(msg); },
        .onTitleChanged = [this](std::string const& title) { set_title(title); }});

    setupMenu();
    setupLayout();

    // Try to restore previous session
    loadSession();
  }

  MainWindow::~MainWindow()
  {
    saveSession();

    // std::jthread auto-joins on destruction, no explicit join needed
  }

  TrackPageContext const* MainWindow::currentVisibleTrackPageContext() const
  {
    return _trackPageGraph->currentVisible();
  }

  TrackPageContext* MainWindow::findTrackPageContext(rs::ListId listId)
  {
    return _trackPageGraph->find(listId);
  }

  void MainWindow::showListPage(rs::ListId listId)
  {
    _trackPageGraph->show(listId);
  }

  void MainWindow::updatePlaybackStatus(rs::audio::PlaybackSnapshot const& snapshot)
  {
    if (_statusBar)
    {
      _statusBar->setPlaybackDetails(snapshot);
    }
  }

  void MainWindow::showPlaybackMessage(std::string const& message, std::optional<std::chrono::seconds> timeout)
  {
    if (_statusBar != nullptr)
    {
      constexpr auto kDefaultStatusTimeout = std::chrono::seconds{5};
      _statusBar->showMessage(message, timeout.value_or(kDefaultStatusTimeout));
    }
  }

  void MainWindow::installLibrarySession(std::unique_ptr<LibrarySession> session)
  {
    auto txn = session->musicLibrary->readTransaction();
    _librarySession = std::move(session);

    if (_tagEditController)
    {
      _tagEditController->setLibrarySession(_librarySession.get());
    }

    if (_statusBar)
    {
      _statusBar->setTrackCount(_librarySession->allTrackIds->size());
    }

    rebuildListPages(txn);

    auto const activeAllTracksListId = allTracksListId();
    _trackPageGraph->show(activeAllTracksListId);

    if (_listSidebarController)
    {
      _listSidebarController->select(activeAllTracksListId);
    }

    saveSession();
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
                                          { _importExportCoordinator->openLibrary(); });
    add_action(openAction);

    auto importAction = Gio::SimpleAction::create("import-files");
    importAction->signal_activate().connect([this]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
                                            { _importExportCoordinator->importFiles(); });
    add_action(importAction);

    auto exportLibAction = Gio::SimpleAction::create("export-library");
    exportLibAction->signal_activate().connect([this]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
                                               { _importExportCoordinator->exportLibrary(); });
    add_action(exportLibAction);

    auto importLibAction = Gio::SimpleAction::create("import-library");
    importLibAction->signal_activate().connect([this]([[maybe_unused]] Glib::VariantBase const& /*variant*/)
                                               { _importExportCoordinator->importLibrary(); });
    add_action(importLibAction);
  }

  void MainWindow::setupLayout()
  {
    // Set up horizontal paned (split view)
    _paned.set_orientation(Gtk::Orientation::HORIZONTAL);
    _paned.set_vexpand(true);

    // Left side: vertical box
    _leftBox.set_orientation(Gtk::Orientation::VERTICAL);
    _leftBox.set_hexpand(true);
    _leftBox.set_vexpand(true);
    _leftBox.set_homogeneous(false);

    // Initialize list sidebar controller
    _listSidebarController = std::make_unique<ListSidebarController>(
      *this,
      ListSidebarController::Callbacks{.onListSelected = [this](rs::ListId listId) { _trackPageGraph->show(listId); },
                                       .onListsChanged =
                                         [this]()
                                       {
                                         if (_librarySession)
                                         {
                                           auto txn = _librarySession->musicLibrary->readTransaction();
                                           rebuildListPages(txn);
                                         }
                                       },
                                       .onListCreatedAndSelected =
                                         [this](rs::ListId listId)
                                       {
                                         if (_librarySession)
                                         {
                                           auto txn = _librarySession->musicLibrary->readTransaction();
                                           rebuildListPages(txn);
                                           _listSidebarController->select(listId);
                                         }
                                       },
                                       .getListMembership = [this](rs::ListId listId) -> rs::model::TrackIdList*
                                       {
                                         if (auto* ctx = _trackPageGraph->find(listId))
                                         {
                                           return ctx->membershipList.get();
                                         }
                                         return nullptr;
                                       }});

    // Add sidebar actions to main window
    _listSidebarController->addActionsTo(*this);

    // Add tag edit actions to main window
    _tagEditController->addActionsTo(*this);

    // Cover art widget (min 50x50)
    _coverArtWidget->set_valign(Gtk::Align::END);
    _coverArtWidget->set_halign(Gtk::Align::FILL);
    _coverArtWidget->set_size_request(kCoverArtSize, kCoverArtSize);
    _coverArtWidget->set_vexpand(false);
    _coverArtWidget->set_hexpand(false);

    // Add widgets to left box
    _listSidebarController->widget().set_vexpand(true);
    _leftBox.append(_listSidebarController->widget());
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

    // Set paned behavior
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

    mainBox->append(_playbackCoordinator->playbackBar());
    mainBox->append(_paned);

    // Status bar at bottom
    _statusBar = std::make_unique<StatusBar>();
    _statusBar->signalNowPlayingClicked().connect(sigc::mem_fun(*this, &MainWindow::jumpToPlayingList));
    _statusBar->signalOutputChanged().connect(sigc::mem_fun(*this, &MainWindow::onOutputChanged));

    auto* statusSeparator = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    mainBox->append(*statusSeparator);
    mainBox->append(*_statusBar);

    // Set as window child
    set_child(*mainBox);
  }

  void MainWindow::rebuildListPages(rs::lmdb::ReadTransaction& txn)
  {
    APP_LOG_DEBUG("rebuildListPages called");

    _trackPageGraph->rebuild(*_librarySession, txn);

    if (_listSidebarController)
    {
      _listSidebarController->rebuildTree(*_librarySession, txn);
    }
  }

  void MainWindow::showStatusMessage(std::string const& message)
  {
    if (_statusBar)
    {
      _statusBar->showMessage(message);
    }
  }

  void MainWindow::updateCoverArt(std::vector<rs::TrackId> const& selectedIds)
  {
    if (!_librarySession || selectedIds.empty())
    {
      // Clear cover art - use default
      _coverArtWidget->clearCover();
      return;
    }

    // Get the first selected track
    auto trackId = selectedIds.front();

    // Look up cover art ID via provider
    auto coverArtId = _librarySession->rowDataProvider->getCoverArtId(trackId);

    if (!coverArtId)
    {
      _coverArtWidget->clearCover();
      return;
    }

    // Load cover art from ResourceStore and display
    rs::lmdb::ReadTransaction txn(_librarySession->musicLibrary->readTransaction());
    auto resourceReader = _librarySession->musicLibrary->resources().reader(txn);
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

  void MainWindow::onTrackSelectionChanged()
  {
    auto* ctx = _trackPageGraph->currentVisible();

    if (ctx == nullptr || ctx->page == nullptr)
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

        if (_librarySession)
        {
          _statusBar->setTrackCount(_librarySession->allTrackIds->size());
        }
      }
      else
      {
        _statusBar->setImportProgress(fraction, info);
      }
    }
  }

  void MainWindow::saveSession()
  {
    _sessionPersistence.save(*this, _paned, _trackColumnLayoutModel, _librarySession.get());
  }

  void MainWindow::loadSession()
  {
    std::string libraryPath;
    rs::audio::BackendKind backendKind = rs::audio::BackendKind::None;
    std::string deviceId;

    _sessionPersistence.load(*this, _paned, _trackColumnLayoutModel, libraryPath, backendKind, deviceId);

    if (backendKind != rs::audio::BackendKind::None)
    {
      if (auto* controller = _playbackCoordinator->playbackController())
      {
        controller->setOutput(backendKind, deviceId);
      }
    }

    if (!libraryPath.empty())
    {
      auto path = std::filesystem::path{libraryPath};
      if (std::filesystem::exists(path / "data.mdb"))
      {
        _importExportCoordinator->openMusicLibrary(path);
      }
    }
  }

  void MainWindow::jumpToPlayingList()
  {
    _playbackCoordinator->jumpToPlayingList();
  }

  void MainWindow::onOutputChanged(rs::audio::BackendKind kind, std::string const& deviceId)
  {
    auto* controller = _playbackCoordinator->playbackController();

    if (controller == nullptr)
    {
      return;
    }

    controller->setOutput(kind, deviceId);
    _statusBar->showMessage("Switched to " + std::string(rs::audio::backendDisplayName(kind)));

    // Persist selection to config
    _sessionPersistence.updateAudioBackend(kind, deviceId);
  }

  std::optional<rs::audio::TrackPlaybackDescriptor> MainWindow::currentSelectionPlaybackDescriptor() const
  {
    auto const* ctx = _trackPageGraph->currentVisible();

    if (ctx == nullptr || ctx->page == nullptr)
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
    return _librarySession->rowDataProvider->getPlaybackDescriptor(*trackId);
  }
} // namespace app::ui

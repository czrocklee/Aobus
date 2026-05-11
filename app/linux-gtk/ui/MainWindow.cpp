// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "MainWindow.h"
#include "PlaybackController.h"
#include "layout/LayoutRuntime.h"
#include "layout/LayoutYaml.h"
#include "layout/editor/LayoutEditorDialog.h"
#include "service/PlaylistExporter.h"
#include <ao/audio/Player.h>
#include <ao/utility/Log.h>
#include <runtime/AppSession.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/NotificationService.h>
#include <runtime/PlaybackService.h>
#include <runtime/ProjectionTypes.h>
#include <runtime/StateTypes.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>
#ifdef ALSA_FOUND
#include <ao/audio/backend/AlsaProvider.h>
#endif
#ifdef PIPEWIRE_FOUND
#include <ao/audio/backend/PipeWireProvider.h>
#endif

#include "CoverArtWidget.h"
#include "ImportProgressDialog.h"
#include "LayoutConstants.h"
#include "ListRow.h"
#include "ListTreeNode.h"
#include "PlaybackBar.h"
#include "SmartListDialog.h"
#include "TagPopover.h"
#include "TrackListAdapter.h"
#include "TrackRowDataProvider.h"
#include "TrackViewPage.h"

#include <glibmm/keyfile.h>
#include <glibmm/variant.h>
#include <gtkmm/filedialog.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <ranges>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    ao::ListId allTracksListId()
    {
      return ao::ListId{std::numeric_limits<std::uint32_t>::max()};
    }

    TrackColumnLayout trackColumnLayoutFromState(TrackViewState const& state)
    {
      auto layout = defaultTrackColumnLayout();
      auto ordered = std::vector<TrackColumnState>{};
      ordered.reserve(layout.columns.size());

      auto takeColumn = [&layout](TrackColumn column) -> std::optional<TrackColumnState>
      {
        auto const it = std::ranges::find(layout.columns, column, &TrackColumnState::column);

        if (it == layout.columns.end())
        {
          return std::nullopt;
        }

        return *it;
      };

      for (auto const& id : state.columnOrder)
      {
        auto const column = trackColumnFromId(id);

        if (!column)
        {
          continue;
        }

        auto const existing = std::ranges::find(ordered, *column, &TrackColumnState::column);

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
        auto const existing = std::ranges::find(ordered, entry.column, &TrackColumnState::column);

        if (existing == ordered.end())
        {
          ordered.push_back(entry);
        }
      }

      layout.columns = std::move(ordered);

      for (auto& entry : layout.columns)
      {
        auto const columnId = std::string{trackColumnId(entry.column)};

        if (std::ranges::contains(state.hiddenColumns, columnId))
        {
          entry.visible = false;
        }

        if (auto const width = state.columnWidths.find(columnId); width != state.columnWidths.end())
        {
          entry.width = width->second;
        }
      }

      return normalizeTrackColumnLayout(layout);
    }

    TrackViewState trackViewStateFromLayout(TrackColumnLayout const& layout)
    {
      auto const normalized = normalizeTrackColumnLayout(layout);
      auto state = TrackViewState{};

      for (auto const& entry : normalized.columns)
      {
        auto const columnId = std::string{trackColumnId(entry.column)};
        state.columnOrder.push_back(columnId);

        if (!entry.visible)
        {
          state.hiddenColumns.push_back(columnId);
        }

        auto const definitionIt =
          std::ranges::find(trackColumnDefinitions(), entry.column, &TrackColumnDefinition::column);

        if (definitionIt != trackColumnDefinitions().end() && entry.width != definitionIt->defaultWidth)
        {
          state.columnWidths.insert_or_assign(columnId, entry.width);
        }
      }

      return state;
    }
  }

  MainWindow::MainWindow(ao::rt::AppSession& session, std::shared_ptr<ao::rt::ConfigStore> configStore)
    : _configStore{std::move(configStore)}
    , _session{session}
    , _componentRegistry{}
    , _componentContext{.registry = _componentRegistry,
                        .session = _session,
                        .parentWindow = *this,
                        .menuModel = {},
                        .onNodeMoved = {}}
    , _layoutHost{_componentRegistry}
  {
    set_title("Aobus");

    // Set default window size
    set_default_size(ao::gtk::kDefaultWindowWidth, ao::gtk::kDefaultWindowHeight);

    // Initialize cover art cache (LRU 100 entries)
    int const coverArtCacheSize = 100;
    _coverArtCache = std::make_unique<CoverArtCache>(coverArtCacheSize);

    // Initialize dispatcher
    _dispatcher = std::make_shared<GtkMainThreadDispatcher>();

    // Initialize StatusBar
    _statusBar = std::make_unique<StatusBar>(_session);

    // Initialize TagEditController
    _tagEditController = std::make_unique<TagEditController>(
      *this,
      _session,
      TagEditController::Callbacks{.onStatusMessage = [this](std::string const& msg) { showStatusMessage(msg); },
                                   .onTagsMutated =
                                     []
                                   {
                                     // Additional invalidation/update logic if needed
                                   }});

    // Initialize list sidebar controller (must exist before TrackPageGraph)
    _listSidebarController = std::make_unique<ListSidebarController>(
      *this,
      _session,
      ListSidebarController::Callbacks{
        .onListSelected = [this](ao::ListId listId) { _session.workspace().navigateTo(listId); },
        .getListMembership = [this](ao::ListId listId) { return &_session.sources().sourceFor(listId); }});
    _componentContext.listSidebarController = _listSidebarController.get();

    // Initialize track page graph
    _trackPageGraph = std::make_unique<TrackPageGraph>(_stack,
                                                       _trackColumnLayoutModel,
                                                       _session,
                                                       _playbackController.get(),
                                                       *_tagEditController,
                                                       *_listSidebarController);

    // Bind inspector sidebar to focused-view detail projection (cover art + audio auto-update)
    // (This is now handled by the InspectorSidebarComponent)

    // Initialize import/export coordinator
    _importExportCoordinator = std::make_unique<ImportExportCoordinator>(
      *this,
      _session,
      ImportExportCallbacks{
        .onOpenNewLibrary =
          [](std::filesystem::path const& /*path*/)
        {
          // Handled externally — opens a new window
        },
        .onLibraryDataMutated =
          [this]
        {
          if (_rowDataProvider)
          {
            _rowDataProvider->loadAll();
            _session.reloadAllTracks();

            auto const txn = _session.musicLibrary().readTransaction();
            rebuildListPages(txn);
            if (_statusBar)
            {
              _statusBar->setTrackCount(_session.sources().allTracks().size());
            }
          }
        },
        .onProgressUpdated = [this](double fraction, std::string const& info) { updateImportProgress(fraction, info); },
        .onStatusMessage = [this](std::string const& msg) { showStatusMessage(msg); },
        .onTitleChanged = [this](std::string const& title) { set_title(title); }});

    // Register standard layout components
    layout::LayoutRuntime::registerStandardComponents(_componentRegistry);

    // Default sidebar state (will be refined by layout later)
    _inspectorHandle.set_active(false);
    _inspectorRevealer.set_reveal_child(false);

    setupMenu();
    setupLayout();

    // Try to restore previous session
    loadSession();
  }

  MainWindow::~MainWindow()
  {
    _tracksMutatedSubscription.reset();
    _importProgressSubscription.reset();
    _importCompletedSubscription.reset();
    _listsMutatedSubscription.reset();

    try
    {
      saveSession();
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to save session in destructor: {}", e.what());
    }
    catch (...)
    {
      APP_LOG_ERROR("Failed to save session in destructor: unknown exception");
    }
  }

  void MainWindow::on_hide()
  {
    saveSession();
    Gtk::ApplicationWindow::on_hide();
  }

  void MainWindow::initializeSession()
  {
    // Create row data provider from AppSession's library
    _rowDataProvider = std::make_unique<TrackRowDataProvider>(_session.musicLibrary());
    _rowDataProvider->loadAll();

    // Populate the all-tracks list from LMDB (replaces old makeLibrarySession's reloadFromStore)
    _session.reloadAllTracks();

    // Register audio backend providers
#ifdef PIPEWIRE_FOUND
    _session.addAudioProvider(std::make_unique<ao::audio::backend::PipeWireProvider>());
#endif
#ifdef ALSA_FOUND
    _session.addAudioProvider(std::make_unique<ao::audio::backend::AlsaProvider>());
#endif

    // Create the shell-side playback controller
    _playbackController = std::make_unique<PlaybackController>(_session, *_rowDataProvider);

    _trackPageGraph->setPlaybackController(*_playbackController);

    // Import progress
    _importProgressSubscription = _session.mutation().onImportProgress(
      [this](auto const& event) { updateImportProgress(event.fraction, event.message); });

    // Import completed — reload data provider (StatusBar handles its own count/progress)
    _importCompletedSubscription = _session.mutation().onImportCompleted(
      [this](auto)
      {
        if (_rowDataProvider)
        {
          _rowDataProvider->loadAll();
          _session.reloadAllTracks();
        }
      });

    // Subscribe to track mutations — invalidate cached row data + notify all views
    _tracksMutatedSubscription = _session.mutation().onTracksMutated(
      [this](auto const& trackIds)
      {
        if (!_rowDataProvider)
        {
          return;
        }

        for (auto const trackId : trackIds)
        {
          _rowDataProvider->invalidate(trackId);
        }

        _session.sources().allTracks().notifyUpdated(trackIds);
      });

    // Subscribe to list mutations
    _listsMutatedSubscription = _session.mutation().onListsMutated(
      [this](auto const& /*event*/)
      {
        if (_rowDataProvider)
        {
          auto const txn = _session.musicLibrary().readTransaction();
          rebuildListPages(txn);
        }
      });

    _tagEditController->setDataProvider(_rowDataProvider.get());

    if (_statusBar)
    {
      _statusBar->showMessage("Aobus Ready");
    }

    // Rebuild layout with all controllers ready
    setupLayoutHost();

    auto const txn = _session.musicLibrary().readTransaction();
    rebuildListPages(txn);

    // Instead of manually showing All Tracks, we restore the session.
    // The session coordinator will either restore previous views or do nothing.
    _session.workspace().restoreSession();

    // If no views were restored, navigate to All Tracks as default
    if (_session.workspace().layoutState().openViews.empty())
    {
      _session.workspace().navigateTo(allTracksListId());
    }

    saveSession();
  }

  void MainWindow::showStatusMessage(std::string_view message)
  {
    _statusBar->showMessage(message);
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

    // View menu
    auto viewMenu = Gio::Menu::create();
    viewMenu->append("Edit Layout...", "win.edit-layout");
    menuModel->append_submenu("View", viewMenu);

    // Help menu (placeholder)
    auto helpMenu = Gio::Menu::create();
    helpMenu->append("About", "app.about");
    menuModel->append_submenu("Help", helpMenu);

    // Store menu model in context for layout component
    _componentContext.menuModel = menuModel;

    // Create actions
    auto openAction = Gio::SimpleAction::create("open-library");
    openAction->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                          { _importExportCoordinator->openLibrary(); });
    add_action(openAction);

    auto importAction = Gio::SimpleAction::create("import-files");
    importAction->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                            { _importExportCoordinator->importFiles(); });
    add_action(importAction);

    auto exportLibAction = Gio::SimpleAction::create("export-library");
    exportLibAction->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                               { _importExportCoordinator->exportLibrary(); });
    add_action(exportLibAction);

    auto importLibAction = Gio::SimpleAction::create("import-library");
    importLibAction->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                               { _importExportCoordinator->importLibrary(); });
    add_action(importLibAction);

    auto editLayoutAction = Gio::SimpleAction::create("edit-layout");
    editLayoutAction->signal_activate().connect([this](Glib::VariantBase const& /*variant*/) { openLayoutEditor(); });
    add_action(editLayoutAction);
  }

  void MainWindow::setupLayout()
  {
    set_child(_layoutHost);

    // Style the handle with CSS
    auto const cssProvider = Gtk::CssProvider::create();
    cssProvider->load_from_data(".inspector-handle {"
                                "  min-width: 14px;"
                                "  padding: 0;"
                                "  margin: 0;"
                                "  border: none;"
                                "  border-radius: 0;"
                                "  background: transparent;"
                                "  transition: background 0.2s;"
                                "}"
                                ".inspector-handle:hover {"
                                "  background: alpha(currentColor, 0.08);"
                                "}"
                                ".inspector-handle image {"
                                "  opacity: 0.4;"
                                "  transition: opacity 0.2s;"
                                "}"
                                ".inspector-handle:hover image {"
                                "  opacity: 1.0;"
                                "}"
                                ".tags-section {"
                                "  margin-top: 4px;"
                                "}"
                                ".tag-chip {"
                                "  border-radius: 100px;"
                                "  padding: 4px 10px;"
                                "  font-size: 0.85rem;"
                                "  font-weight: 500;"
                                "  transition: all 0.2s ease;"
                                "}"
                                "togglebutton.tag-chip {"
                                "  background: alpha(currentColor, 0.05);"
                                "  border: 1px solid transparent;"
                                "  color: alpha(currentColor, 0.7);"
                                "}"
                                "togglebutton.tag-chip:checked {"
                                "  background: alpha(currentColor, 0.15);"
                                "  color: currentColor;"
                                "  border-color: alpha(currentColor, 0.1);"
                                "}"
                                "togglebutton.tag-chip:hover {"
                                "  background: alpha(currentColor, 0.2);"
                                "}"
                                ".tag-remove-button {"
                                "  min-width: 18px;"
                                "  min-height: 18px;"
                                "  padding: 0;"
                                "  margin-left: 4px;"
                                "  border-radius: 100px;"
                                "  background: transparent;"
                                "  border: none;"
                                "  opacity: 0.4;"
                                "  transition: opacity 0.2s;"
                                "}"
                                ".tag-remove-button:hover {"
                                "  opacity: 1.0;"
                                "  background: alpha(@error_color, 0.1);"
                                "}"
                                ".tags-entry {"
                                "  background: alpha(currentColor, 0.05);"
                                "  border: 1px solid transparent;"
                                "  border-radius: 8px;"
                                "  padding: 6px 12px;"
                                "  margin-top: 8px;"
                                "  transition: all 0.2s;"
                                "  font-size: 0.9rem;"
                                "}"
                                ".tags-entry:focus {"
                                "  border-color: alpha(@accent_color, 0.5);"
                                "  background: alpha(currentColor, 0.08);"
                                "  box-shadow: none;"
                                "}");
    Gtk::StyleContext::add_provider_for_display(
      Gdk::Display::get_default(), cssProvider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  void MainWindow::openLayoutEditor()
  {
    auto const dialog = std::make_shared<layout::editor::LayoutEditorDialog>(*this, _componentRegistry, _activeLayout);
    auto* const dialogPtr = dialog.get();

    _componentContext.editMode = true;
    _componentContext.onNodeMoved = [dialogPtr](std::string const& nodeId, int posX, int posY)
    { dialogPtr->updateNodePosition(nodeId, posX, posY); };

    // Rebuild to inject edit mode
    _layoutHost.setLayout(_componentContext, _activeLayout);

    dialogPtr->signalApplyPreview().connect([this](layout::LayoutDocument const& doc)
                                            { _layoutHost.setLayout(_componentContext, doc); });

    dialogPtr->signal_response().connect(
      [this, sharedDialog = dialog](int responseId)
      {
        _componentContext.editMode = false;
        _componentContext.onNodeMoved = nullptr;

        if (responseId == Gtk::ResponseType::OK)
        {
          _activeLayout = sharedDialog->document();
          _layoutHost.setLayout(_componentContext, _activeLayout);

          _configStore->save("linuxGtkLayout", _activeLayout);
        }
        else if (responseId == Gtk::ResponseType::CANCEL)
        {
          // Revert preview
          _layoutHost.setLayout(_componentContext, _activeLayout);
        }

        sharedDialog->close();
      });

    dialogPtr->present();
  }

  void MainWindow::setupLayoutHost()
  {
    // Try to load layout from config
    auto doc = layout::createDefaultLayout();
    _configStore->load("linuxGtkLayout", doc);

    // Update context with initialized controllers
    _componentContext.rowDataProvider = _rowDataProvider.get();
    _componentContext.coverArtCache = _coverArtCache.get();
    _componentContext.playbackController = _playbackController.get();
    _componentContext.tagEditController = _tagEditController.get();
    _componentContext.importExportCoordinator = _importExportCoordinator.get();
    _componentContext.trackPageGraph = _trackPageGraph.get();
    _componentContext.columnLayoutModel = &_trackColumnLayoutModel;
    _componentContext.statusBar = _statusBar.get();

    _activeLayout = doc;
    _layoutHost.setLayout(_componentContext, _activeLayout);
  }

  void MainWindow::rebuildListPages(ao::lmdb::ReadTransaction const& txn)
  {
    APP_LOG_DEBUG("rebuildListPages called");

    _trackPageGraph->rebuild(*_rowDataProvider, txn);

    if (_listSidebarController)
    {
      _listSidebarController->rebuildTree(*_rowDataProvider, txn);
    }
  }

  void MainWindow::updateImportProgress(double fraction, std::string_view info)
  {
    if (_statusBar)
    {
      if (fraction >= 1.0)
      {
        _statusBar->clearImportProgress();

        if (_rowDataProvider)
        {
          _statusBar->setTrackCount(_session.sources().allTracks().size());
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
    auto ws = WindowState{};
    if (auto const width = get_width(); width > 0)
    {
      ws.width = width;
    }

    if (auto const height = get_height(); height > 0)
    {
      ws.height = height;
    }

    ws.maximized = is_maximized();

    // Paned position will be handled by layout persistence in the future.
    // For Phase 3, we just remove the direct paned access.

    _configStore->save("window", ws);

    _configStore->save("linuxGtkLayout", _activeLayout);

    _configStore->save("track_view", trackViewStateFromLayout(_trackColumnLayoutModel.layout()));

    _session.workspace().saveSession();
  }

  void MainWindow::loadSession()
  {
    auto ws = WindowState{};
    _configStore->load("window", ws);

    set_default_size(ws.width, ws.height);

    if (ws.maximized)
    {
      maximize();
    }

    auto tvs = TrackViewState{};
    _configStore->load("track_view", tvs);
    _trackColumnLayoutModel.setLayout(trackColumnLayoutFromState(tvs));
  }
} // namespace ao::gtk

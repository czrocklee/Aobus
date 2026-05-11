// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "MainWindow.h"
#include "PlaybackController.h"
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
      auto normalized = normalizeTrackColumnLayout(layout);
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

  MainWindow::MainWindow(ao::app::AppSession& session, std::shared_ptr<ao::app::ConfigStore> configStore)
    : _configStore{std::move(configStore)}, _session{session}, _coverArtWidget{nullptr}
  {
    set_title("Aobus");

    // Set default window size
    set_default_size(ao::gtk::kDefaultWindowWidth, ao::gtk::kDefaultWindowHeight);

    // Initialize cover art cache (LRU 100 entries)
    _coverArtCache = std::make_unique<CoverArtCache>(100);

    // Initialize cover art widget and bind to focused-view detail projection
    _coverArtWidget = std::make_unique<CoverArtWidget>(_session, *_coverArtCache);
    _coverArtWidget->bindToDetailProjection(_session.views().detailProjection(ao::app::FocusedViewTarget{}));

    // Initialize dispatcher
    _dispatcher = std::make_shared<GtkMainThreadDispatcher>();

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
        .getListMembership = [](ao::ListId /*listId*/) -> ao::app::TrackSource*
        {
          // This callback is slightly problematic in the new architecture
          // as the sidebar shouldn't care about membership lists directly.
          // For now, we still allow it but it's a candidate for future cleanup.
          return nullptr;
        }});

    // Initialize track page graph
    _trackPageGraph = std::make_unique<TrackPageGraph>(_stack,
                                                       _trackColumnLayoutModel,
                                                       _session,
                                                       _playbackController.get(),
                                                       *_tagEditController,
                                                       *_listSidebarController);

    // Initialize inspector sidebar
    _inspectorSidebar = std::make_unique<InspectorSidebar>(_session, *_coverArtCache);
    _inspectorSidebar->signalTagEditRequested().connect(
      [this](std::vector<ao::TrackId> const& ids, Gtk::Widget* relativeTo)
      {
        if (!_rowDataProvider || relativeTo == nullptr)
        {
          return;
        }

        auto const selection = TrackSelectionContext{.listId = allTracksListId(), .selectedIds = ids};

        _tagEditController->showTagEditor(selection, *relativeTo);
      });

    // Bind inspector sidebar to focused-view detail projection (cover art + audio auto-update)
    _inspectorSidebar->bindToDetailProjection(_session.views().detailProjection(ao::app::FocusedViewTarget{}));

    // Initialize playback bar (self-wires to AppSession)
    _playbackBar = std::make_unique<PlaybackBar>(_session);

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

    setupMenu();
    setupLayout();

    // Try to restore previous session
    loadSession();

    // Default sidebar state
    _inspectorHandle.set_active(false);
    _inspectorRevealer.set_reveal_child(false);
    _inspectorSidebar->set_visible(false);
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
      _statusBar->setTrackCount(_session.sources().allTracks().size());
    }

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

    // Help menu (placeholder)
    auto helpMenu = Gio::Menu::create();
    helpMenu->append("About", "app.about");
    menuModel->append_submenu("Help", helpMenu);

    // Set up menu bar
    _menuBar.set_menu_model(menuModel);

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

    // Right side: stack for pages and inspector
    _stack.set_hexpand(true);
    _stack.set_vexpand(true);

    // Inspector container
    auto* inspectorBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    inspectorBox->set_hexpand(true);
    inspectorBox->set_vexpand(true);

    _inspectorRevealer.set_transition_type(Gtk::RevealerTransitionType::SLIDE_LEFT);
    _inspectorRevealer.set_child(*_inspectorSidebar);
    _inspectorRevealer.set_reveal_child(false);
    _inspectorRevealer.set_hexpand(false);
    _inspectorRevealer.set_vexpand(true);
    _inspectorSidebar->set_vexpand(true);

    // Inspector handle (sidebar trigger)
    _inspectorHandle.set_icon_name("pan-start-symbolic");
    _inspectorHandle.add_css_class("inspector-handle");
    _inspectorHandle.set_valign(Gtk::Align::CENTER);
    _inspectorHandle.set_hexpand(false);
    _inspectorHandle.set_focus_on_click(false);

    _inspectorHandle.signal_toggled().connect(
      [this]
      {
        bool const active = _inspectorHandle.get_active();
        _inspectorRevealer.set_reveal_child(active);

        // Ensure the sidebar doesn't take space when collapsed
        _inspectorSidebar->set_visible(active);

        _inspectorHandle.set_icon_name(active ? "pan-end-symbolic" : "pan-start-symbolic");
        _inspectorHandle.set_tooltip_text(active ? "Hide Inspector" : "Show Inspector");
      });

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

    inspectorBox->append(_stack);
    inspectorBox->append(_inspectorHandle);
    inspectorBox->append(_inspectorRevealer);

    // Add placeholder page
    auto* placeholderLabel = Gtk::make_managed<Gtk::Label>("Select a library or import tracks");
    placeholderLabel->set_halign(Gtk::Align::CENTER);
    placeholderLabel->set_valign(Gtk::Align::CENTER);
    _stack.add(*placeholderLabel, "welcome", "Welcome");

    // Add to paned
    _paned.set_start_child(_leftBox);
    _paned.set_end_child(*inspectorBox);

    // Set paned behavior
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

    mainBox->append(*_playbackBar);
    mainBox->append(_paned);

    // Status bar at bottom
    _statusBar = std::make_unique<StatusBar>(_session);
    auto* statusSeparator = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    mainBox->append(*statusSeparator);
    mainBox->append(*_statusBar);

    // Set as window child
    set_child(*mainBox);
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

    if (auto const pos = _paned.get_position(); pos > 0)
    {
      ws.panedPosition = pos;
    }

    _configStore->save("window", ws);

    _configStore->save("track_view", trackViewStateFromLayout(_trackColumnLayoutModel.layout()));

    _session.workspace().saveSession();
  }

  void MainWindow::loadSession()
  {
    auto ws = WindowState{};
    _configStore->load("window", ws);

    set_default_size(ws.width, ws.height);

    if (ws.panedPosition > 0)
    {
      _paned.set_position(ws.panedPosition);
    }

    if (ws.maximized)
    {
      maximize();
    }

    auto tvs = TrackViewState{};
    _configStore->load("track_view", tvs);
    _trackColumnLayoutModel.setLayout(trackColumnLayoutFromState(tvs));
  }
} // namespace ao::gtk

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "app/AppConfigStore.h"
#include "app/AppDialog.h"
#include "app/GtkAccelTranslator.h"
#include "app/GtkLayoutStateStore.h"
#include "app/KeymapApplicator.h"
#include "app/MenuController.h"
#include "app/ShellLayoutCollaborators.h"
#include "app/ShellLayoutComponentStateStore.h"
#include "app/ThemeCoordinator.h"
#include "app/WindowActionRegistry.h"
#include "app/WindowInput.h"
#include "app/WindowState.h"
#include "i18n/GtkText.h"
#include "image/ImageCache.h"
#include "image/ResourceImageLoader.h"
#include "list/ListNavigationController.h"
#include "platform/MprisArtUrlCache.h"
#include "platform/MprisBridge.h"
#include "portal/ImportExportCallbacks.h"
#include "portal/ImportExportCoordinator.h"
#include "tag/TagEditController.h"
#include "track/TrackOrderActions.h"
#include "track/TrackPageHost.h"
#include "track/TrackRowCache.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/AppState.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/playback/output/OutputSelection.h>
#include <ao/uimodel/preference/ThemePreset.h>
#include <ao/utility/Path.h>
#include <ao/utility/ScopedRegistration.h>

#include <gdkmm/enums.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/dialog.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::gtk
{
  struct MainWindow::Impl final
  {
    Impl(Gtk::Window& window, rt::AppRuntime& runtime, i18n::MessageCatalog catalog)
      : layoutStateStore{rt::LibraryPaths{runtime.musicRoot()}.managedDataPath()}
      , trackRowCache{runtime.library(), catalog}
      , imageCache{100}
      , resourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()}
      , playbackActions{runtime.playback(), [&runtime] { std::ignore = runtime.playSelectionInFocusedView(); }}
      , textCatalog{std::move(catalog)}
      , trackPresentationCatalog{runtime.workspace(), textCatalog}
      , listPresentations{trackPresentationCatalog, runtime.library().changes()}
      , trackColumnLayouts{runtime.library().changes()}
      , tagEditController{window,
                          runtime,
                          textCatalog,
                          TagEditController::Callbacks{
                            .onTagsMutated = [] {},
                            .onManageListsRequested = [this] { listNavigationController.openNewPlaylistDialog(); },
                          },
                          themeCoordinator}
      , listNavigationController{window,
                                 runtime,
                                 textCatalog,
                                 ListNavigationController::Callbacks{
                                   .onListSelected = [&runtime, this](ListId listId)
                                   { return navigateToList(listId, runtime).has_value(); },
                                   .onListPresentationSaved = [this](ListId listId, std::string const& presentationId)
                                   { listPresentations.setPresentationIdForList(listId, presentationId); },
                                   .listPresentationCallback = [this](ListId listId) -> std::optional<std::string>
                                   {
                                     if (auto const optPres = listPresentations.presentationIdForList(listId); optPres)
                                     {
                                       return std::string{*optPres};
                                     }

                                     return std::nullopt;
                                   }},
                                 themeCoordinator}
      , trackPageHost{stack,
                      runtime,
                      tagEditController,
                      listNavigationController,
                      trackColumnLayouts,
                      textCatalog,
                      runtime.resourceBytes()}
      , importExportCoordinator{window,
                                runtime,
                                textCatalog,
                                portal::ImportExportCallbacks{
                                  .onOpenNewLibrary = [](std::filesystem::path const&, bool) {},
                                  .onTitleChanged = [&window](std::string const& title) { window.set_title(title); }},
                                themeCoordinator}
    {
      tagEditController.setDataProvider(&trackRowCache);
    }

    rt::TrackPresentationSpec presentationForList(ListId listId, rt::AppRuntime& runtime) const
    {
      auto const fallback = [this, listId]
      {
        return listPresentations.presentationForList(uimodel::ListPresentationContext{
          .listId = listId,
          .sourceKind = uimodel::ListPresentationSourceKind::AllTracks,
        });
      };

      if (rt::isVirtualListId(listId))
      {
        return fallback();
      }

      auto const optNode = runtime.library().snapshot().listNode(listId);

      if (!optNode)
      {
        return fallback();
      }

      return listPresentations.presentationForList(uimodel::ListPresentationContext{
        .listId = listId,
        .sourceKind = uimodel::ListPresentationSourceKind::SavedList,
        .listExpression = optNode->expression,
      });
    }

    Result<rt::ViewId> navigateToList(ListId listId, rt::AppRuntime& runtime) const
    {
      auto const spec = presentationForList(listId, runtime);
      return runtime.workspace().navigate({
        .target = listId,
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::NewViewDefault,
            .spec = spec,
          },
      });
    }

    void restorePlaybackSession(rt::AppRuntime& runtime) const
    {
      auto restoredRes = runtime.restorePlaybackSession();

      if (!restoredRes)
      {
        APP_LOG_WARN("MainWindow: Failed to restore playback session - {}", restoredRes.error().message);
        return;
      }

      if (!restoredRes->restored)
      {
        return;
      }

      std::ignore = navigateToList(restoredRes->sourceListId, runtime);
      runtime.playback().commands().revealTrack(restoredRes->trackId, rt::kInvalidViewId, restoredRes->sourceListId);
    }

    GtkLayoutStateStore layoutStateStore;
    ThemeCoordinator themeCoordinator;
    TrackRowCache trackRowCache;
    ImageCache imageCache;
    ResourceImageLoader resourceImageLoader;
    uimodel::PlaybackActions playbackActions;
    i18n::MessageCatalog textCatalog;
    ao::uimodel::TrackPresentationCatalog trackPresentationCatalog;
    ao::uimodel::ListPresentations listPresentations;
    ao::uimodel::TrackColumnLayouts trackColumnLayouts;
    TagEditController tagEditController;
    ListNavigationController listNavigationController;
    Gtk::Stack stack;
    TrackPageHost trackPageHost;
    portal::ImportExportCoordinator importExportCoordinator;

    WindowState windowState;
    std::optional<ThemeRegistrationToken> optThemeToken;
    bool restoringLayoutState = false;

    async::Subscription listsMutatedSub;
    async::Subscription trackPresentationChangedSub;
    async::Subscription trackColumnLayoutChangedSub;
  };

  MainWindow::MainWindow(rt::AppRuntime& runtime,
                         std::shared_ptr<AppConfigStore> configStorePtr,
                         std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
                         i18n::MessageCatalog textCatalog,
                         std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr)
    : _runtime{runtime}
    , _configStorePtr{std::move(configStorePtr)}
    , _textCatalog{std::move(textCatalog)}
    , _implPtr{std::make_unique<Impl>(*this, _runtime, _textCatalog)}
    , _menuControllerPtr{std::make_unique<MenuController>(_textCatalog)}
    , _shellLayout{_runtime,
                   *this,
                   _configStorePtr,
                   std::move(shellLayoutStorePtr),
                   std::move(componentStateStorePtr),
                   shellLayoutCollaborators(_menuControllerPtr->menuModel())}
  {
    set_title("Aobus");
    set_default_size(kDefaultWindowWidth, kDefaultWindowHeight);

    _implPtr->trackPresentationChangedSub = _implPtr->listPresentations.signalChanged().connect(
      [this](ao::ListId /*listId*/) { saveColumnLayoutIfNotRestoring(); });
    _implPtr->trackColumnLayoutChangedSub = _implPtr->trackColumnLayouts.signalChanged().connect(
      [this](ao::ListId /*listId*/) { saveColumnLayoutIfNotRestoring(); });

    loadSessionState();

    _windowActionRegistryPtr = std::make_unique<WindowActionRegistry>(
      _implPtr->importExportCoordinator,
      WindowActionRegistry::Callbacks{
        .onEditLayout = [this] { openLayoutEditor(); },
        .onResetRuntimeLayoutState = [this] { resetRuntimeLayoutState(); },
        .onSaveCurrentPanelSizesAsLayoutDefaults = [this] { saveCurrentPanelSizesAsLayoutDefaults(); },
      });
    _windowActionsRegistration = _windowActionRegistryPtr->install(*this);
    _listNavigationActionsRegistration = _implPtr->listNavigationController.addActionsTo(*this);
    _shellLayout.attachToWindow();

    auto mprisArtUrlCachePtr = std::make_shared<platform::MprisArtUrlCache>(_runtime.resourceBytes(), _runtime.async());
    _mprisBridgePtr = std::make_unique<platform::MprisBridge>(
      _runtime.playback(),
      _implPtr->playbackActions,
      platform::MprisBridge::Callbacks{
        .raise =
          [this]
        {
          present();
          return true;
        },
        .quit =
          [this]
        {
          if (auto const appPtr = get_application(); appPtr)
          {
            appPtr->quit();
            return true;
          }

          return false;
        },
        .requestArtUrl = [cachePtr = std::move(mprisArtUrlCachePtr)](
                           ResourceId const resourceId, platform::MprisBridge::OnArtUrlReady onReady)
        { return cachePtr->requestUrl(resourceId, std::move(onReady)); },
      });
    _shellLayout.setConfirmPromotionCallback(
      [this](std::string const& presetId, ShellLayoutController::ConfirmPromotionAnswer answer)
      {
        AppDialog::presentMessage(
          *this,
          gtkText(_textCatalog, i18n::MessageId::GtkShellSavePanelSizesAsLayoutDefaults),
          i18n::requiredFormat(_textCatalog, i18n::MessageId::GtkSaveLayoutDefaultsMessage, {{"preset", presetId}}),
          {AppDialogAction{.label = gtkText(_textCatalog, i18n::MessageId::GtkCommonNo),
                           .responseId = Gtk::ResponseType::NO,
                           .role = AppDialogActionRole::Cancel},
           AppDialogAction{.label = gtkText(_textCatalog, i18n::MessageId::GtkCommonYes),
                           .responseId = Gtk::ResponseType::YES,
                           .role = AppDialogActionRole::Primary}},
          Gtk::ResponseType::NO,
          [answer = std::move(answer)](std::int32_t const responseId) mutable
          { answer(responseId == Gtk::ResponseType::YES); });
      });

    _keymap = _configStorePtr->loadKeymap(uimodel::defaultKeymap());
    installPlaybackSpaceShortcut();
    installOrderKeyRepeatSuppression();

    // Mouse back/forward navigation (thumb buttons 8/9).
    auto mouseNavGesturePtr = Gtk::GestureClick::create();
    mouseNavGesturePtr->set_button(0); // listen to all buttons

    mouseNavGesturePtr->signal_pressed().connect(
      [this, mouseNavGesturePtr](std::int32_t /*nPress*/, double /*x*/, double /*y*/)
      {
        auto const optNavigation =
          mouseButtonNavigation(static_cast<std::int32_t>(mouseNavGesturePtr->get_current_button()));

        if (optNavigation == WorkspaceNavigation::Back)
        {
          std::ignore = _runtime.workspace().goBack();
        }
        else if (optNavigation == WorkspaceNavigation::Forward)
        {
          std::ignore = _runtime.workspace().goForward();
        }
      });
    add_controller(mouseNavGesturePtr);
  }

  MainWindow::~MainWindow()
  {
    try
    {
      saveSession();
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "GTK MainWindow session save during destruction");
    }

    // Close window action callbacks before either their producers or the
    // inherited action map can retire. Member order provides the same fallback
    // for constructor unwinding.
    _listNavigationActionsRegistration.reset();
    _windowActionsRegistration.reset();

    // Stop observing before the collaborators the callbacks read are released.
    // Member order would release them in the same order, but a subscription is
    // a registration on a live source, not a member whose order proves it is
    // gone; the window states its retirement rather than implying it.
    _implPtr->listsMutatedSub.reset();
    _implPtr->trackPresentationChangedSub.reset();
    _implPtr->trackColumnLayoutChangedSub.reset();
  }

  void MainWindow::saveSession()
  {
    if (_sessionPhase != SessionPhase::Active)
    {
      return;
    }

    auto const persistenceReady = _playbackPersistenceAdmission == PlaybackPersistenceAdmission::Ready;
    auto const policy = persistenceReady ? SessionSavePolicy::Full : SessionSavePolicy::ExcludeSelectedRootAndPlayback;
    recordWindowGeometry(
      _implPtr->windowState, WindowState{.width = get_width(), .height = get_height(), .maximized = is_maximized()});
    saveSessionState(policy);
  }

  Result<> MainWindow::retireForLibrarySwitch()
  {
    if (_sessionPhase == SessionPhase::Retired)
    {
      return {};
    }

    AO_EXPECTS(_sessionPhase == SessionPhase::Active, "Only an active GTK session can be retired");

    saveSession();

    if (auto discardedRes = _runtime.retirePlaybackSessionForLibrarySwitch(); !discardedRes)
    {
      APP_LOG_ERROR("Failed to retire active library for process restart: {}", discardedRes.error().message);
      auto* const dialog =
        AppDialog::presentMessage(*this,
                                  gtkText(_textCatalog, i18n::MessageId::GtkUnableSwitchLibraries),
                                  discardedRes.error().message,
                                  {AppDialogAction{.label = gtkText(_textCatalog, i18n::MessageId::GtkCommonClose),
                                                   .responseId = Gtk::ResponseType::CLOSE,
                                                   .role = AppDialogActionRole::Cancel}},
                                  Gtk::ResponseType::CLOSE);

      auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_implPtr->themeCoordinator.registerToplevel(*dialog));
      dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });

      return discardedRes;
    }

    _sessionPhase = SessionPhase::Retired;
    return {};
  }

  std::filesystem::path const& MainWindow::musicRoot() const noexcept
  {
    return _runtime.musicRoot();
  }

  MainWindow::SessionPhase MainWindow::sessionPhase() const noexcept
  {
    return _sessionPhase;
  }

  bool MainWindow::isMprisStarted() const noexcept
  {
    return _mprisStarted;
  }

  void MainWindow::on_hide()
  {
    saveSession();
    Gtk::ApplicationWindow::on_hide();
  }

  void MainWindow::size_allocate_vfunc(int const width, int const height, int const baseline)
  {
    Gtk::ApplicationWindow::size_allocate_vfunc(width, height, baseline);

    if (_implPtr)
    {
      recordWindowGeometry(
        _implPtr->windowState, WindowState{.width = width, .height = height, .maximized = is_maximized()});
    }
  }

  portal::ImportExportCoordinator& MainWindow::importExportCoordinator()
  {
    return _implPtr->importExportCoordinator;
  }

  Result<> MainWindow::prepareSession()
  {
    AO_EXPECTS(_sessionPhase == SessionPhase::Constructed, "Only a constructed GTK session can be prepared");

    prepareRuntimeSession();

    _shellLayout.refreshExportedActions();

    _shellLayout.loadLayout();
    _sessionPhase = SessionPhase::Prepared;
    return {};
  }

  Result<> MainWindow::activateSession(PlaybackRestoreMode const restoreMode)
  {
    AO_EXPECTS(_sessionPhase == SessionPhase::Prepared, "Only a prepared GTK session can be activated");

    _playbackPersistenceAdmission = restoreMode == PlaybackRestoreMode::Restore
                                      ? PlaybackPersistenceAdmission::Ready
                                      : PlaybackPersistenceAdmission::AwaitingRootCommit;
    _sessionPhase = SessionPhase::Active;

    if (restoreMode == PlaybackRestoreMode::Restore)
    {
      _runtime.startPlaybackSessionPersistence();
      restorePlaybackSession();
    }

    try
    {
      _mprisBridgePtr->start();
      _mprisStarted = true;
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_WARN("Failed to activate MPRIS for GTK session: {}", e.what());
    }

    return {};
  }

  Result<> MainWindow::commitSuccessorLibrarySelection()
  {
    AO_EXPECTS(_sessionPhase == SessionPhase::Active, "Only an active GTK successor can commit its library");
    AO_EXPECTS(_playbackPersistenceAdmission == PlaybackPersistenceAdmission::AwaitingRootCommit,
               "GTK successor library selection can only be committed once");

    auto appSession = rt::AppSessionState{};
    _configStorePtr->loadAppSession(appSession);
    appSession.lastLibraryPath = utility::pathToUtf8(_runtime.musicRoot());
    auto persistedRes = _configStorePtr->saveAppSession(appSession);

    if (!persistedRes)
    {
      _runtime.sealPlaybackSessionPersistenceWrites();
      _playbackPersistenceAdmission = PlaybackPersistenceAdmission::Sealed;
      return persistedRes;
    }

    _runtime.startPlaybackSessionPersistence();
    _playbackPersistenceAdmission = PlaybackPersistenceAdmission::Ready;
    return {};
  }

  void MainWindow::rebuildLayout()
  {
    _shellLayout.loadLayout();
  }

  void MainWindow::openLayoutEditor()
  {
    _shellLayout.openEditor(*_configStorePtr);
  }

  void MainWindow::resetRuntimeLayoutState()
  {
    _shellLayout.resetRuntimeLayoutState();
  }

  void MainWindow::saveCurrentPanelSizesAsLayoutDefaults()
  {
    _shellLayout.saveCurrentPanelSizesAsLayoutDefaults();
  }

  void MainWindow::applyKeymap(uimodel::KeymapModel const& keymap)
  {
    _configStorePtr->saveKeymap(keymap);
    _keymap = keymap;
    _orderKeyRepeatGuard.reset();

    if (auto const appPtr = get_application(); appPtr)
    {
      applyKeymapAccelerators(*appPtr, keymap);
    }
  }

  void MainWindow::applyTheme(uimodel::ThemePreset const theme)
  {
    _implPtr->themeCoordinator.setTheme(theme);
  }

  rt::PlaybackService& MainWindow::playback()
  {
    return _runtime.playback();
  }

  uimodel::LayoutSchema const& MainWindow::layoutSchema() const
  {
    return _shellLayout.layoutSchema();
  }

  void MainWindow::prepareRuntimeSession()
  {
    // Track-row coherence is the cache's own business; the window only needs to
    // know when the list tree it shows has to be rebuilt.
    _implPtr->listsMutatedSub = _runtime.library().changes().onChanged(
      [this](rt::LibraryChangeSet const& mutation)
      {
        if (!mutation.libraryReset && mutation.listsUpserted.empty() && mutation.listsDeleted.empty() &&
            mutation.listOrderChanges.empty())
        {
          return;
        }

        rebuildListPages();
      });

    auto const restoredRes = _runtime.workspace().restoreSession(_runtime.workspaceConfigStore());

    if (!restoredRes)
    {
      APP_LOG_WARN("MainWindow: Failed to restore workspace session - {}", restoredRes.error().message);
    }

    if (_runtime.workspace().snapshot().openViews.empty())
    {
      std::ignore = _implPtr->navigateToList(rt::kAllTracksListId, _runtime);
    }

    rebuildListPages();
  }

  Result<rt::ViewId> MainWindow::navigateToList(ListId const listId)
  {
    return _implPtr->navigateToList(listId, _runtime);
  }

  void MainWindow::restorePlaybackSession()
  {
    _implPtr->restorePlaybackSession(_runtime);
  }

  void MainWindow::saveSessionState(SessionSavePolicy const policy)
  {
    auto const fullSave = policy == SessionSavePolicy::Full;

    _configStorePtr->saveWindow(_implPtr->windowState);

    saveColumnLayout();

    // Session state: per-window shutdown must not overwrite explicit application preferences.
    auto session = rt::AppSessionState{};
    _configStorePtr->loadAppSession(session);

    if (fullSave)
    {
      session.lastLibraryPath = utility::pathToUtf8(_runtime.musicRoot());
    }

    session.lastOutputSelection = _runtime.playback().snapshot().transport.output.selectedDevice;

    if (auto const savedRes = _configStorePtr->saveAppSession(session); !savedRes)
    {
      APP_LOG_WARN("MainWindow: Failed to save application session - {}", savedRes.error().message);
    }

    if (fullSave)
    {
      if (auto const savedRes = _runtime.savePlaybackSession(); !savedRes)
      {
        APP_LOG_WARN("MainWindow: Failed to checkpoint playback session - {}", savedRes.error().message);
      }
    }

    _runtime.workspace().saveSession(_runtime.workspaceConfigStore());
  }

  void MainWindow::loadSessionState()
  {
    // Window state
    _configStorePtr->loadWindow(_implPtr->windowState);
    set_default_size(_implPtr->windowState.width, _implPtr->windowState.height);

    if (_implPtr->windowState.maximized)
    {
      maximize();
    }

    // Column layouts (widths and order)
    auto columnState = ao::uimodel::TrackColumnLayouts::Snapshot{};
    auto prefState = ao::uimodel::ListPresentations::Snapshot{};
    _implPtr->layoutStateStore.load(columnState, prefState);
    _implPtr->restoringLayoutState = true;
    auto const restoreGuard = utility::ScopedRegistration{[this] { _implPtr->restoringLayoutState = false; }};
    _implPtr->trackColumnLayouts.restore(columnState);
    _implPtr->listPresentations.restore(prefState);

    // App prefs (playback restoration)
    auto prefs = rt::AppPrefsState{};
    _configStorePtr->loadAppPrefs(prefs);
    auto session = rt::AppSessionState{};
    _configStorePtr->loadAppSession(session);

    auto& playback = _runtime.playback();
    auto const optOutputSelection = uimodel::resolveOutputDeviceSelectionToRestore(
      prefs.preferredOutputSelection, session.lastOutputSelection, playback.snapshot().transport.output);

    if (optOutputSelection)
    {
      playback.commands().setOutputDevice(
        optOutputSelection->backendId, optOutputSelection->deviceId, optOutputSelection->profileId);
    }

    _implPtr->themeCoordinator.load(*_configStorePtr);
    _implPtr->optThemeToken = _implPtr->themeCoordinator.registerToplevel(*this);
  }

  ShellLayoutCollaborators MainWindow::shellLayoutCollaborators(Glib::RefPtr<Gio::MenuModel> menuModelPtr)
  {
    return ShellLayoutCollaborators{
      .textCatalog = _implPtr->textCatalog,
      .playbackActions = &_implPtr->playbackActions,
      .themeCoordinator = &_implPtr->themeCoordinator,
      .trackRowCache = &_implPtr->trackRowCache,
      .imageLoader = &_implPtr->resourceImageLoader,
      .tagEditController = &_implPtr->tagEditController,
      .importExportActions = &_implPtr->importExportCoordinator,
      .trackPageHost = &_implPtr->trackPageHost,
      .trackPresentationCatalog = &_implPtr->trackPresentationCatalog,
      .listPresentations = &_implPtr->listPresentations,
      .listNavigationController = &_implPtr->listNavigationController,
      .outputDeviceIntent = preferredOutputDeviceRecorder(_configStorePtr),
      .createSmartListFromExpression = [navigationController = &_implPtr->listNavigationController](
                                         ao::ListId parentListId, std::string expression)
      { navigationController->createSmartListFromExpression(parentListId, std::move(expression)); },
      .menuModelPtr = std::move(menuModelPtr),
    };
  }

  void MainWindow::rebuildListPages()
  {
    APP_LOG_DEBUG("rebuildListPages called");
    _implPtr->trackPageHost.rebuild(_implPtr->trackRowCache);

    _implPtr->listNavigationController.rebuildTree(_implPtr->trackRowCache);
  }

  void MainWindow::saveColumnLayout()
  {
    auto const columnState = _implPtr->trackColumnLayouts.snapshot();
    auto const prefState = _implPtr->listPresentations.snapshot();

    _implPtr->layoutStateStore.save(columnState, prefState);
  }

  void MainWindow::saveColumnLayoutIfNotRestoring()
  {
    if (!_implPtr->restoringLayoutState)
    {
      saveColumnLayout();
    }
  }

  void MainWindow::installPlaybackSpaceShortcut()
  {
    auto keyControllerPtr = Gtk::EventControllerKey::create();
    keyControllerPtr->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType modifiers)
      {
        if (!shouldActivatePlaybackSpaceShortcut(keyval, modifiers, get_focus()))
        {
          return false;
        }

        _shellLayout.activateAction("playback.playPause");
        return true;
      },
      false);
    add_controller(keyControllerPtr);
  }

  void MainWindow::installOrderKeyRepeatSuppression()
  {
    auto keyControllerPtr = Gtk::EventControllerKey::create();
    keyControllerPtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keyControllerPtr->signal_key_pressed().connect(
      [this](guint const keyval, guint const keycode, Gdk::ModifierType const modifiers)
      {
        auto const optChord = fromGtkKeyval(keyval, modifiers);

        if (!optChord)
        {
          return false;
        }

        auto const optActionId = _keymap.actionFor(*optChord);

        if (!optActionId || !isTrackOrderAction(*optActionId))
        {
          return false;
        }

        if (auto const physicalKeycode = keycode == 0 ? keyval : keycode;
            isSinglePressTrackOrderAction(*optActionId) && !_orderKeyRepeatGuard.acceptPress(physicalKeycode))
        {
          return true;
        }

        _shellLayout.activateAction(*optActionId);
        return true;
      },
      false);
    keyControllerPtr->signal_key_released().connect(
      [this](guint const keyval, guint const keycode, Gdk::ModifierType)
      {
        auto const physicalKeycode = keycode == 0 ? keyval : keycode;
        _orderKeyRepeatGuard.release(physicalKeycode);
      });
    add_controller(keyControllerPtr);

    property_is_active().signal_changed().connect(
      [this]
      {
        if (!property_is_active().get_value())
        {
          _orderKeyRepeatGuard.reset();
        }
      });
  }
} // namespace ao::gtk

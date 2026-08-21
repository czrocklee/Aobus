// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "app/AppConfigStore.h"
#include "app/AppDialog.h"
#include "app/GtkAccelTranslator.h"
#include "app/KeymapApplicator.h"
#include "app/MainWindowCoordinator.h"
#include "app/MenuController.h"
#include "app/MouseNavigationPolicy.h"
#include "app/PlaybackShortcutPolicy.h"
#include "app/ShellLayoutComponentStateStore.h"
#include "app/ThemeCoordinator.h"
#include "app/WindowActionRegistry.h"
#include "app/WindowState.h"
#include "i18n/GtkTextCatalog.h"
#include "list/ListNavigationController.h"
#include "platform/MprisArtUrlCache.h"
#include "platform/MprisBridge.h"
#include "portal/ImportExportCoordinator.h"
#include "track/TrackOrderActions.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/preference/ThemePreset.h>
#include <ao/utility/Path.h>

#include <gdkmm/enums.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/dialog.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::gtk
{
  MainWindow::MainWindow(rt::AppRuntime& runtime,
                         std::shared_ptr<AppConfigStore> configStorePtr,
                         std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
                         uimodel::PresentationTextCatalog textCatalog,
                         GtkTextCatalog const& gtkTextCatalog,
                         std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr)
    : _runtime{runtime}
    , _configStorePtr{std::move(configStorePtr)}
    , _textCatalog{std::move(textCatalog)}
    , _mainWindowCoordinatorPtr{std::make_unique<MainWindowCoordinator>(*this,
                                                                        _runtime,
                                                                        _configStorePtr,
                                                                        _textCatalog,
                                                                        gtkTextCatalog)}
    , _shellLayout{_runtime,
                   *this,
                   _configStorePtr,
                   std::move(shellLayoutStorePtr),
                   std::move(componentStateStorePtr),
                   _mainWindowCoordinatorPtr->uiDependencies()}
  {
    set_title("Aobus");
    set_default_size(kDefaultWindowWidth, kDefaultWindowHeight);

    _mainWindowCoordinatorPtr->loadSession();

    _windowActionRegistryPtr = std::make_unique<WindowActionRegistry>(
      _mainWindowCoordinatorPtr->importExport(),
      WindowActionRegistry::Callbacks{
        .onEditLayout = [this] { openLayoutEditor(); },
        .onResetRuntimeLayoutState = [this] { resetRuntimeLayoutState(); },
        .onSaveCurrentPanelSizesAsLayoutDefaults = [this] { saveCurrentPanelSizesAsLayoutDefaults(); },
      });
    _windowActionRegistryPtr->install(*this);

    _menuControllerPtr = std::make_unique<MenuController>();
    _menuControllerPtr->setup(gtkTextCatalog);
    _mainWindowCoordinatorPtr->listNavigationController()->addActionsTo(*this);
    _shellLayout.setMenuModel(_menuControllerPtr->menuModel());
    _shellLayout.attachToWindow();

    auto mprisArtUrlCachePtr =
      std::make_shared<platform::MprisArtUrlCache>(*_mainWindowCoordinatorPtr->resourceByteLoader(), _runtime.async());
    _mprisBridgePtr = std::make_unique<platform::MprisBridge>(
      _runtime.playback(),
      *_mainWindowCoordinatorPtr->playbackCommandSurface(),
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
          std::string{_textCatalog.text(i18n::MessageId::GtkShellSavePanelSizesAsLayoutDefaults)},
          _textCatalog.format(i18n::MessageId::GtkSaveLayoutDefaultsMessage, {{"preset", presetId}}),
          {AppDialogAction{.label = std::string{_textCatalog.text(i18n::MessageId::GtkCommonNo)},
                           .responseId = Gtk::ResponseType::NO,
                           .role = AppDialogActionRole::Cancel},
           AppDialogAction{.label = std::string{_textCatalog.text(i18n::MessageId::GtkCommonYes)},
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
  }

  void MainWindow::saveSession()
  {
    if (_sessionPhase != SessionPhase::Active)
    {
      return;
    }

    auto const persistenceReady = _playbackPersistenceAdmission == PlaybackPersistenceAdmission::Ready;
    auto const policy = persistenceReady ? MainWindowCoordinator::SessionSavePolicy::Full
                                         : MainWindowCoordinator::SessionSavePolicy::ExcludeSelectedRootAndPlayback;
    _mainWindowCoordinatorPtr->saveSession(policy);
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
      auto* const dialog = AppDialog::presentMessage(
        *this,
        std::string{_textCatalog.text(i18n::MessageId::GtkUnableSwitchLibraries)},
        discardedRes.error().message,
        {AppDialogAction{.label = std::string{_textCatalog.text(i18n::MessageId::GtkCommonClose)},
                         .responseId = Gtk::ResponseType::CLOSE,
                         .role = AppDialogActionRole::Cancel}},
        Gtk::ResponseType::CLOSE);

      if (auto* const themeCoordinator = _mainWindowCoordinatorPtr->themeCoordinator(); themeCoordinator != nullptr)
      {
        auto tokenPtr = std::make_shared<ThemeRegistrationToken>(themeCoordinator->registerToplevel(*dialog));
        dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
      }

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

  portal::ImportExportCoordinator& MainWindow::importExportCoordinator()
  {
    return _mainWindowCoordinatorPtr->importExport();
  }

  Result<> MainWindow::prepareSession()
  {
    AO_EXPECTS(_sessionPhase == SessionPhase::Constructed, "Only a constructed GTK session can be prepared");

    _mainWindowCoordinatorPtr->prepareSession();

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
      _mainWindowCoordinatorPtr->restorePlaybackSession();
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
    if (auto* const themeCoordinator = _mainWindowCoordinatorPtr->themeCoordinator(); themeCoordinator != nullptr)
    {
      themeCoordinator->setTheme(theme);
    }
  }

  rt::PlaybackService& MainWindow::playback()
  {
    return _runtime.playback();
  }

  uimodel::LayoutActionCatalog const& MainWindow::layoutActionCatalog() const
  {
    return _shellLayout.actionCatalog();
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

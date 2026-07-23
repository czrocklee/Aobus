// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "app/AppConfigStore.h"
#include "app/AppDialog.h"
#include "app/KeymapApplicator.h"
#include "app/MainWindowCoordinator.h"
#include "app/MenuController.h"
#include "app/MouseNavigationPolicy.h"
#include "app/PlaybackShortcutPolicy.h"
#include "app/ShellLayoutComponentStateStore.h"
#include "app/ThemeCoordinator.h"
#include "app/WindowActionRegistry.h"
#include "app/WindowState.h"
#include "list/ListNavigationController.h"
#include "platform/MprisArtUrlCache.h"
#include "platform/MprisBridge.h"
#include "portal/ImportExportCoordinator.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <gdkmm/enums.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/dialog.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
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
                         std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr)
    : _runtime{runtime}
    , _configStorePtr{std::move(configStorePtr)}
    , _mainWindowCoordinatorPtr{std::make_unique<MainWindowCoordinator>(*this, _runtime, _configStorePtr)}
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
    _menuControllerPtr->setup();
    _mainWindowCoordinatorPtr->listNavigationController()->addActionsTo(*this);
    _shellLayout.setMenuModel(_menuControllerPtr->menuModel());
    _shellLayout.attachToWindow();

    auto mprisArtUrlCachePtr =
      std::make_shared<platform::MprisArtUrlCache>(_runtime.library().taskService(), _runtime.async());
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
          "Save Current Panel Sizes as Layout Defaults?",
          std::format("This will update the '{}' layout preset on disk with the current panel sizes.\n\n"
                      "Promoted sizes will be removed from runtime state; other runtime values such as revealed state "
                      "will remain.",
                      presetId),
          {AppDialogAction{.label = "No", .responseId = Gtk::ResponseType::NO, .role = AppDialogActionRole::Cancel},
           AppDialogAction{.label = "Yes", .responseId = Gtk::ResponseType::YES, .role = AppDialogActionRole::Primary}},
          Gtk::ResponseType::NO,
          [answer = std::move(answer)](std::int32_t const responseId) mutable
          { answer(responseId == Gtk::ResponseType::YES); });
      });

    installPlaybackSpaceShortcut();

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
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to save runtime in destructor: {}", e.what());
    }
    catch (...)
    {
      APP_LOG_ERROR("Failed to save runtime in destructor: unknown exception");
    }
  }

  void MainWindow::saveSession()
  {
    if (_sessionPhase != SessionPhase::Active)
    {
      return;
    }

    _mainWindowCoordinatorPtr->saveSession();
  }

  Result<> MainWindow::retireForLibrarySwitch()
  {
    if (_sessionPhase == SessionPhase::Retired)
    {
      return {};
    }

    if (_sessionPhase != SessionPhase::Active)
    {
      return makeError(Error::Code::InvalidState, "Only an active GTK session can be retired");
    }

    saveSession();

    if (auto discarded = _runtime.discardRestorablePlaybackSession(); !discarded)
    {
      APP_LOG_ERROR("Failed to retire active library for replacement: {}", discarded.error().message);
      auto* const dialog = AppDialog::presentMessage(
        *this,
        "Unable to Switch Libraries",
        discarded.error().message,
        {AppDialogAction{
          .label = "Close", .responseId = Gtk::ResponseType::CLOSE, .role = AppDialogActionRole::Cancel}},
        Gtk::ResponseType::CLOSE);

      if (auto* const themeCoordinator = _mainWindowCoordinatorPtr->themeCoordinator(); themeCoordinator != nullptr)
      {
        auto tokenPtr = std::make_shared<ThemeRegistrationToken>(themeCoordinator->registerToplevel(*dialog));
        dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
      }

      return discarded;
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
    if (_sessionPhase != SessionPhase::Constructed)
    {
      return makeError(Error::Code::InvalidState, "Only a constructed GTK session can be prepared");
    }

    _mainWindowCoordinatorPtr->prepareSession();

    _shellLayout.refreshExportedActions();

    _shellLayout.loadLayout();
    _sessionPhase = SessionPhase::Prepared;
    return {};
  }

  Result<> MainWindow::activateSession(PlaybackRestoreMode const restoreMode)
  {
    if (_sessionPhase != SessionPhase::Prepared)
    {
      return makeError(Error::Code::InvalidState, "Only a prepared GTK session can be activated");
    }

    _sessionPhase = SessionPhase::Active;

    if (restoreMode == PlaybackRestoreMode::Restore)
    {
      try
      {
        _mainWindowCoordinatorPtr->restorePlaybackSession();
      }
      catch (std::exception const& e)
      {
        APP_LOG_WARN("Failed to restore GTK playback session during activation: {}", e.what());
      }
      catch (...)
      {
        APP_LOG_WARN("Failed to restore GTK playback session during activation: unknown exception");
      }
    }

    try
    {
      _mprisBridgePtr->start();
      _mprisStarted = true;
    }
    catch (std::exception const& e)
    {
      APP_LOG_WARN("Failed to activate MPRIS for GTK session: {}", e.what());
    }
    catch (...)
    {
      APP_LOG_WARN("Failed to activate MPRIS for GTK session: unknown exception");
    }

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
} // namespace ao::gtk

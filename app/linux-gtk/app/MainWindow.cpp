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

    auto mprisArtUrlCachePtr = std::make_shared<platform::MprisArtUrlCache>(_runtime.library());
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
        .artUrlForResource = [cachePtr = std::move(mprisArtUrlCachePtr)](ResourceId const resourceId)
        { return cachePtr->urlForResource(resourceId); },
      });
    _mprisBridgePtr->start();

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
    if (_librarySwitchPrepared)
    {
      return;
    }

    _mainWindowCoordinatorPtr->saveSession();
  }

  Result<> MainWindow::prepareForLibrarySwitch()
  {
    if (_librarySwitchPrepared)
    {
      return {};
    }

    saveSession();

    if (auto discarded = _runtime.discardRestorablePlaybackSession(); !discarded)
    {
      return discarded;
    }

    _librarySwitchPrepared = true;
    return {};
  }

  std::filesystem::path const& MainWindow::musicRoot() const noexcept
  {
    return _runtime.musicRoot();
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

  void MainWindow::initializeSession()
  {
    _mainWindowCoordinatorPtr->initializeSession();

    _shellLayout.refreshExportedActions();

    _shellLayout.loadLayout(*_configStorePtr);
  }

  void MainWindow::rebuildLayout()
  {
    _shellLayout.loadLayout(*_configStorePtr);
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

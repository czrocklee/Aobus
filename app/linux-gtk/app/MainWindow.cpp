// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "app/AppConfig.h"
#include "app/KeyboardShortcutsWindow.h"
#include "app/KeymapApplicator.h"
#include "app/MainWindowCoordinator.h"
#include "app/MenuController.h"
#include "app/MouseNavigationPolicy.h"
#include "app/PlaybackShortcutPolicy.h"
#include "app/ShellLayoutComponentStateStore.h"
#include "app/UIState.h"
#include "portal/ImportExportCoordinator.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <gdkmm/enums.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/messagedialog.h>

#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <utility>

namespace ao::gtk
{
  MainWindow::MainWindow(rt::AppRuntime& runtime,
                         std::shared_ptr<AppConfig> configPtr,
                         std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
                         std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr)
    : _runtime{runtime}
    , _configPtr{std::move(configPtr)}
    , _mainWindowCoordinatorPtr{std::make_unique<MainWindowCoordinator>(*this, _runtime, _configPtr)}
    , _shellLayout{_runtime,
                   *this,
                   _configPtr,
                   std::move(shellLayoutStorePtr),
                   std::move(componentStateStorePtr),
                   *_mainWindowCoordinatorPtr->themeController()}
  {
    set_title("Aobus");
    set_default_size(kDefaultWindowWidth, kDefaultWindowHeight);

    _mainWindowCoordinatorPtr->loadSession();

    _menuControllerPtr = std::make_unique<MenuController>(
      _mainWindowCoordinatorPtr->importExport(),
      [this] { _shellLayout.openEditor(*_configPtr); },
      [this] { _shellLayout.resetRuntimeLayoutState(); },
      [this] { _shellLayout.saveCurrentPanelSizesAsLayoutDefaults(); },
      [this] { openKeyboardShortcutsWindow(); });
    _menuControllerPtr->setup(*this);
    _shellLayout.context().shell.menuModelPtr = _menuControllerPtr->menuModel();

    _shellLayout.attachToWindow();

    _shellLayout.setConfirmPromotionCallback(
      [this](std::string const& presetId, ShellLayoutController::ConfirmPromotionAnswer answer)
      {
        auto dialogPtr = std::make_shared<Gtk::MessageDialog>(*this,
                                                              "Save Current Panel Sizes as Layout Defaults?",
                                                              false,
                                                              Gtk::MessageType::QUESTION,
                                                              Gtk::ButtonsType::YES_NO,
                                                              true);
        dialogPtr->set_secondary_text(std::format(
          "This will update the '{}' layout preset on disk with the current panel sizes.\n\n"
          "Promoted sizes will be removed from runtime state; other runtime values such as revealed state will remain.",
          presetId));

        dialogPtr->set_default_response(Gtk::ResponseType::NO);

        dialogPtr->signal_response().connect(
          [dialogPtr, answer = std::move(answer)](std::int32_t const responseId) mutable
          {
            answer(responseId == Gtk::ResponseType::YES);
            dialogPtr->close();
          });
        dialogPtr->present();
      });

    setupPlaybackSpaceShortcut();

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
          _runtime.workspace().goBack();
        }
        else if (optNavigation == WorkspaceNavigation::Forward)
        {
          _runtime.workspace().goForward();
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
    _mainWindowCoordinatorPtr->saveSession();
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

    _shellLayout.context().bind(_mainWindowCoordinatorPtr->uiServices());
    _shellLayout.refreshExportedActions();

    _shellLayout.loadLayout(*_configPtr);
  }

  void MainWindow::rebuildLayout()
  {
    _shellLayout.loadLayout(*_configPtr);
  }

  void MainWindow::setupPlaybackSpaceShortcut()
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

  void MainWindow::openKeyboardShortcutsWindow()
  {
    if (_keyboardShortcutsWindowPtr)
    {
      _keyboardShortcutsWindowPtr->present();
      return;
    }

    // The window is owned by MainWindow for its whole lifetime: closing it merely hides it, and a
    // later open re-presents the same instance. This keeps teardown deterministic (the unique_ptr
    // destroys it with MainWindow, while the type is complete here) and avoids deleting a window
    // from inside its own hide signal or leaving a callback that could outlive MainWindow.
    _keyboardShortcutsWindowPtr =
      std::make_unique<KeyboardShortcutsWindow>(_shellLayout.actionCatalog(),
                                                _configPtr->loadKeymap(uimodel::defaultKeymap()),
                                                [this](uimodel::KeymapModel const& keymap)
                                                {
                                                  _configPtr->saveKeymap(keymap);

                                                  if (auto const appPtr = get_application(); appPtr)
                                                  {
                                                    applyKeymapAccelerators(*appPtr, keymap);
                                                  }
                                                });

    _keyboardShortcutsWindowPtr->set_transient_for(*this);
    _keyboardShortcutsWindowPtr->present();
  }
} // namespace ao::gtk

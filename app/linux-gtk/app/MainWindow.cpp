// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "app/AppConfig.h"
#include "app/MainWindowCoordinator.h"
#include "app/MenuController.h"
#include "app/UIState.h"
#include "portal/ImportExportCoordinator.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/utility/Log.h>

#include <gtkmm/applicationwindow.h>
#include <gtkmm/gestureclick.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

namespace ao::gtk
{
  MainWindow::MainWindow(rt::AppRuntime& runtime, std::shared_ptr<AppConfig> configPtr)
    : _runtime{runtime}
    , _configPtr{std::move(configPtr)}
    , _mainWindowCoordinatorPtr{std::make_unique<MainWindowCoordinator>(*this, _runtime, _configPtr)}
    , _shellLayout{_runtime, *this, _configPtr}
  {
    set_title("Aobus");
    set_default_size(kDefaultWindowWidth, kDefaultWindowHeight);

    _mainWindowCoordinatorPtr->loadSession();

    _menuControllerPtr = std::make_unique<MenuController>(
      _mainWindowCoordinatorPtr->importExport(), [this] { _shellLayout.openEditor(*_configPtr); });
    _menuControllerPtr->setup(*this);
    _shellLayout.context().shell.menuModelPtr = _menuControllerPtr->menuModel();

    _shellLayout.attachToWindow();

    // Mouse back/forward navigation (buttons 8/9).
    auto mouseNavGesturePtr = Gtk::GestureClick::create();
    mouseNavGesturePtr->set_button(0); // listen to all buttons
    constexpr int kMouseBackButton = 8;
    constexpr int kMouseForwardButton = 9;

    mouseNavGesturePtr->signal_pressed().connect(
      [this, mouseNavGesturePtr](std::int32_t /*nPress*/, double /*x*/, double /*y*/)
      {
        if (auto const button = mouseNavGesturePtr->get_current_button(); button == kMouseBackButton)
        {
          _runtime.workspace().goBack();
        }
        else if (button == kMouseForwardButton)
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
      _mainWindowCoordinatorPtr->saveSession();
      _shellLayout.saveLayout(*_configPtr);
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

  void MainWindow::on_hide()
  {
    _mainWindowCoordinatorPtr->saveSession();
    _shellLayout.saveLayout(*_configPtr);
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
} // namespace ao::gtk

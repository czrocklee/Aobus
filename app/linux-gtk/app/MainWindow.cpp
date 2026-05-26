// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "ao/utility/Log.h"
#include "app/AppConfig.h"
#include "app/MainWindowCoordinator.h"
#include "app/MenuController.h"
#include "app/UIState.h"
#include "portal/ImportExportCoordinator.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/WorkspaceService.h>

#include <gtkmm/applicationwindow.h>
#include <gtkmm/gestureclick.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

namespace ao::gtk
{
  MainWindow::MainWindow(rt::AppRuntime& runtime, std::shared_ptr<AppConfig> config)
    : _runtime{runtime}
    , _config{std::move(config)}
    , _mainWindowCoordinator{std::make_unique<MainWindowCoordinator>(*this, _runtime, _config)}
    , _shellLayout{_runtime, *this}
  {
    set_title("Aobus");
    set_default_size(kDefaultWindowWidth, kDefaultWindowHeight);

    _mainWindowCoordinator->loadSession();

    _menuController = std::make_unique<MenuController>(
      _mainWindowCoordinator->importExport(), [this] { _shellLayout.openEditor(*_config); });
    _menuController->setup(*this);
    _shellLayout.context().shell.menuModel = _menuController->menuModel();

    _shellLayout.attachToWindow();

    // Mouse back/forward navigation (buttons 8/9).
    auto mouseNavGesture = Gtk::GestureClick::create();
    mouseNavGesture->set_button(0); // listen to all buttons
    constexpr int kMouseBackButton = 8;
    constexpr int kMouseForwardButton = 9;

    mouseNavGesture->signal_pressed().connect(
      [this, mouseNavGesture](std::int32_t /*nPress*/, double /*x*/, double /*y*/)
      {
        if (auto const button = mouseNavGesture->get_current_button(); button == kMouseBackButton)
        {
          _runtime.workspace().goBack();
        }
        else if (button == kMouseForwardButton)
        {
          _runtime.workspace().goForward();
        }
      });
    add_controller(mouseNavGesture);
  }

  MainWindow::~MainWindow()
  {
    try
    {
      _mainWindowCoordinator->saveSession();
      _shellLayout.saveLayout(*_config);
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
    _mainWindowCoordinator->saveSession();
    _shellLayout.saveLayout(*_config);
    Gtk::ApplicationWindow::on_hide();
  }

  portal::ImportExportCoordinator& MainWindow::importExportCoordinator()
  {
    return _mainWindowCoordinator->importExport();
  }

  void MainWindow::initializeSession()
  {
    _mainWindowCoordinator->initializeSession();

    _shellLayout.context().bind(_mainWindowCoordinator->uiServices());

    _shellLayout.loadLayout(*_config);
  }

  void MainWindow::rebuildLayout()
  {
    _shellLayout.loadLayout(*_config);
  }
} // namespace ao::gtk

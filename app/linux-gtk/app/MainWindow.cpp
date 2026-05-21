// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "ao/utility/Log.h"
#include "app/MainWindowCoordinator.h"
#include "app/MenuController.h"
#include "app/UIState.h"
#include "portal/ImportExportCoordinator.h"
#include "runtime/AppRuntime.h"
#include "runtime/ConfigStore.h"

#include <gtkmm/applicationwindow.h>

#include <exception>
#include <memory>
#include <utility>

namespace ao::gtk
{
  MainWindow::MainWindow(rt::AppRuntime& runtime,
                         std::shared_ptr<rt::ConfigStore> globalConfig,
                         std::shared_ptr<rt::ConfigStore> workspaceConfig)
    : _runtime{runtime}
    , _globalConfig{std::move(globalConfig)}
    , _workspaceConfig{std::move(workspaceConfig)}
    , _mainWindowCoordinator{std::make_unique<MainWindowCoordinator>(*this, _runtime, _globalConfig, _workspaceConfig)}
    , _shellLayout{_runtime, *this}
  {
    set_title("Aobus");
    set_default_size(kDefaultWindowWidth, kDefaultWindowHeight);

    _mainWindowCoordinator->loadSession();

    _menuController = std::make_unique<MenuController>(
      _mainWindowCoordinator->importExport(), [this] { _shellLayout.openEditor(*_globalConfig); });
    _menuController->setup(*this);
    _shellLayout.context().shell.menuModel = _menuController->menuModel();

    _shellLayout.attachToWindow();
  }

  MainWindow::~MainWindow()
  {
    try
    {
      _mainWindowCoordinator->saveSession();
      _shellLayout.saveLayout(*_globalConfig);
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
    _shellLayout.saveLayout(*_globalConfig);
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

    _shellLayout.loadLayout(*_globalConfig);
  }

  void MainWindow::rebuildLayout()
  {
    _shellLayout.loadLayout(*_globalConfig);
  }
} // namespace ao::gtk

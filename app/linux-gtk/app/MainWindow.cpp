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
  MainWindow::MainWindow(rt::AppRuntime& runtime, std::shared_ptr<rt::ConfigStore> configStore)
    : _runtime{runtime}
    , _configStore{std::move(configStore)}
    , _mainWindowCoordinator{std::make_unique<MainWindowCoordinator>(*this, _runtime, _configStore)}
    , _shellLayout{_runtime, *this}
  {
    set_title("Aobus");
    set_default_size(kDefaultWindowWidth, kDefaultWindowHeight);

    _mainWindowCoordinator->loadSession();

    _menuController = std::make_unique<MenuController>(
      _mainWindowCoordinator->importExport(), [this] { _shellLayout.openEditor(*_configStore); });
    _menuController->setup(*this);
    _shellLayout.context().shell.menuModel = _menuController->menuModel();

    _shellLayout.attachToWindow();
  }

  MainWindow::~MainWindow()
  {
    try
    {
      _mainWindowCoordinator->saveSession();
      _shellLayout.saveLayout(*_configStore);
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
    _shellLayout.saveLayout(*_configStore);
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

    _shellLayout.loadLayout(*_configStore);
  }

  void MainWindow::rebuildLayout()
  {
    _shellLayout.loadLayout(*_configStore);
  }
} // namespace ao::gtk

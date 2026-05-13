// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"
#include "app/MenuController.h"
#include "app/WindowController.h"
#include "layout/LayoutConstants.h"
#include <ao/utility/Log.h>
#include <exception>
#include <runtime/AppSession.h>
#include <runtime/ConfigStore.h>

namespace ao::gtk
{
  MainWindow::MainWindow(ao::rt::AppSession& session, std::shared_ptr<ao::rt::ConfigStore> configStore)
    : _session{session}
    , _configStore{std::move(configStore)}
    , _windowController{std::make_unique<WindowController>(*this, _session, _configStore)}
    , _shellLayout{_session, *this}
  {
    set_title("Aobus");
    set_default_size(ao::gtk::kDefaultWindowWidth, ao::gtk::kDefaultWindowHeight);

    _windowController->loadSession();

    _menuController = std::make_unique<MenuController>(
      _windowController->importExport(), [this] { _shellLayout.openEditor(*_configStore); });
    _menuController->setup(*this);
    _shellLayout.context().shell.menuModel = _menuController->menuModel();

    _shellLayout.attachToWindow();
  }

  MainWindow::~MainWindow()
  {
    try
    {
      _windowController->saveSession();
      _shellLayout.saveLayout(*_configStore);
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
    _windowController->saveSession();
    _shellLayout.saveLayout(*_configStore);
    Gtk::ApplicationWindow::on_hide();
  }

  ImportExportCoordinator& MainWindow::importExportCoordinator()
  {
    return _windowController->importExport();
  }

  void MainWindow::initializeSession()
  {
    _windowController->initializeSession();

    auto& ctx = _shellLayout.context();
    ctx.track.trackRowCache = _windowController->trackRowCache();
    ctx.inspector.coverArtCache = _windowController->coverArtCache();
    ctx.playback.sequenceController = _windowController->playbackSequenceController();
    ctx.tag.editController = _windowController->tagEditController();
    ctx.libraryIo.coordinator = _windowController->importExportCoordinator();
    ctx.track.pageManager = _windowController->trackPageManager();
    ctx.track.columnLayoutModel = _windowController->columnLayoutModel();
    ctx.list.sidebarController = _windowController->listSidebarController();

    _shellLayout.loadLayout(*_configStore);
  }

  void MainWindow::rebuildLayout()
  {
    _shellLayout.loadLayout(*_configStore);
  }
} // namespace ao::gtk

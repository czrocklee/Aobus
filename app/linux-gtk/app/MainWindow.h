// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ShellLayoutController.h"
#include "library_io/ImportExportCoordinator.h"

#include <gtkmm.h>
#include <memory>

namespace ao::rt
{
  class AppSession;
  class ConfigStore;
}

namespace ao::gtk
{
  class MenuController;
  class WindowController;

  class MainWindow final : public Gtk::ApplicationWindow
  {
  public:
    explicit MainWindow(ao::rt::AppSession& session, std::shared_ptr<ao::rt::ConfigStore> configStore);
    ~MainWindow() override;

    void on_hide() override;

    ImportExportCoordinator& importExportCoordinator();

    void initializeSession();
    void rebuildLayout();

  private:
    ao::rt::AppSession& _session;
    std::shared_ptr<ao::rt::ConfigStore> _configStore;

    std::unique_ptr<WindowController> _windowController;
    ShellLayoutController _shellLayout;
    std::unique_ptr<MenuController> _menuController;
  };
} // namespace ao::gtk

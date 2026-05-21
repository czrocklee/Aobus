// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ShellLayoutController.h"

#include <gtkmm/applicationwindow.h>

#include <memory>

namespace ao::rt
{
  class AppRuntime;
  class ConfigStore;
}

namespace ao::gtk
{
  class MenuController;
  class MainWindowCoordinator;
  namespace portal
  {
    class ImportExportCoordinator;
  }

  class MainWindow final : public Gtk::ApplicationWindow
  {
  public:
    explicit MainWindow(rt::AppRuntime& runtime,
                        std::shared_ptr<rt::ConfigStore> globalConfig,
                        std::shared_ptr<rt::ConfigStore> workspaceConfig);
    ~MainWindow() override;

    MainWindow(MainWindow const&) = delete;
    MainWindow& operator=(MainWindow const&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    void on_hide() override;

    portal::ImportExportCoordinator& importExportCoordinator();

    void initializeSession();
    void rebuildLayout();

  private:
    rt::AppRuntime& _runtime;
    std::shared_ptr<rt::ConfigStore> _globalConfig;
    std::shared_ptr<rt::ConfigStore> _workspaceConfig;

    std::unique_ptr<MainWindowCoordinator> _mainWindowCoordinator;
    ShellLayoutController _shellLayout;
    std::unique_ptr<MenuController> _menuController;
  };
} // namespace ao::gtk

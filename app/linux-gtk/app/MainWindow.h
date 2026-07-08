// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ShellLayoutController.h"
#include <ao/rt/AppPrefsState.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>

#include <gtkmm/applicationwindow.h>

#include <memory>

namespace ao::rt
{
  class AppRuntime;
  class PlaybackService;
}

namespace ao::gtk
{
  class AppConfig;
  class ShellLayoutComponentStateStore;
  class ShellLayoutStore;
  class MenuController;
  class MainWindowCoordinator;
  class WindowActionRegistry;
  namespace portal
  {
    class ImportExportCoordinator;
  }
  namespace platform
  {
    class MprisBridge;
  }

  class MainWindow final : public Gtk::ApplicationWindow
  {
  public:
    explicit MainWindow(rt::AppRuntime& runtime,
                        std::shared_ptr<AppConfig> configPtr,
                        std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
                        std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr = nullptr);
    ~MainWindow() override;

    MainWindow(MainWindow const&) = delete;
    MainWindow& operator=(MainWindow const&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    void saveSession();
    void on_hide() override;

    portal::ImportExportCoordinator& importExportCoordinator();

    void initializeSession();
    void rebuildLayout();
    void openLayoutEditor();
    void resetRuntimeLayoutState();
    void saveCurrentPanelSizesAsLayoutDefaults();
    void applyKeymap(uimodel::KeymapModel const& keymap);
    void applyTheme(rt::ThemePresetId theme);
    rt::PlaybackService& playbackService();
    uimodel::LayoutActionCatalog const& layoutActionCatalog() const;

  private:
    void installPlaybackSpaceShortcut();

    rt::AppRuntime& _runtime;
    std::shared_ptr<AppConfig> _configPtr;

    std::unique_ptr<MainWindowCoordinator> _mainWindowCoordinatorPtr;
    ShellLayoutController _shellLayout;
    std::unique_ptr<WindowActionRegistry> _windowActionRegistryPtr;
    std::unique_ptr<MenuController> _menuControllerPtr;
    std::unique_ptr<platform::MprisBridge> _mprisBridgePtr;
  };
} // namespace ao::gtk

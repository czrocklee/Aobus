// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ShellLayoutController.h"
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/input/KeyRepeatGuard.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <gtkmm/applicationwindow.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::rt
{
  class AppRuntime;
  class PlaybackService;
}

namespace ao::gtk
{
  class AppConfigStore;
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
    enum class PlaybackRestoreMode : std::uint8_t
    {
      Restore,
      StartIdle,
    };

    enum class SessionPhase : std::uint8_t
    {
      Constructed,
      Prepared,
      Active,
      Retired,
    };

    explicit MainWindow(rt::AppRuntime& runtime,
                        std::shared_ptr<AppConfigStore> configStorePtr,
                        std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
                        i18n::MessageCatalog textCatalog,
                        std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr = nullptr);
    ~MainWindow() override;

    MainWindow(MainWindow const&) = delete;
    MainWindow& operator=(MainWindow const&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    void saveSession();
    Result<> prepareSession();
    Result<> activateSession(PlaybackRestoreMode restoreMode);
    Result<> commitSuccessorLibrarySelection();
    Result<> retireForLibrarySwitch();
    std::filesystem::path const& musicRoot() const noexcept;
    SessionPhase sessionPhase() const noexcept;
    bool isMprisStarted() const noexcept;

    portal::ImportExportCoordinator& importExportCoordinator();

    void rebuildLayout();
    void openLayoutEditor();
    void resetRuntimeLayoutState();
    void saveCurrentPanelSizesAsLayoutDefaults();
    void applyKeymap(uimodel::KeymapModel const& keymap);
    void applyTheme(uimodel::ThemePreset theme);
    rt::PlaybackService& playback();
    uimodel::LayoutActionCatalog const& layoutActionCatalog() const;

  protected:
    void on_hide() override;
    void size_allocate_vfunc(int width, int height, int baseline) override;

  private:
    enum class PlaybackPersistenceAdmission : std::uint8_t
    {
      Ready,
      AwaitingRootCommit,
      Sealed,
    };

    void installPlaybackSpaceShortcut();
    void installOrderKeyRepeatSuppression();

    rt::AppRuntime& _runtime;
    std::shared_ptr<AppConfigStore> _configStorePtr;
    i18n::MessageCatalog _textCatalog;

    std::unique_ptr<MainWindowCoordinator> _mainWindowCoordinatorPtr;
    ShellLayoutController _shellLayout;
    std::unique_ptr<WindowActionRegistry> _windowActionRegistryPtr;
    std::unique_ptr<MenuController> _menuControllerPtr;
    std::unique_ptr<platform::MprisBridge> _mprisBridgePtr;
    uimodel::KeymapModel _keymap;
    uimodel::KeyRepeatGuard _orderKeyRepeatGuard;
    SessionPhase _sessionPhase = SessionPhase::Constructed;
    PlaybackPersistenceAdmission _playbackPersistenceAdmission = PlaybackPersistenceAdmission::Ready;
    bool _mprisStarted = false;
  };
} // namespace ao::gtk

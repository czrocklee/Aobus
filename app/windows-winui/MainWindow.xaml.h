// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "MainWindow.g.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/Credits.h>
#include <ao/utility/ScopedRegistration.h>

#include <bitset>
// The loaded theme override is held here, so its type is part of the frame.
#include <ao/winui/Theme.h>
#include <ao/winui/layout/ShellState.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ao::uimodel
{
  class TransportViewModel;
}

namespace ao::winui
{
  class CoverArtPresenter;
  class LibrarySession;
  class LibraryTransferCoordinator;
  class ListAuthoringCoordinator;
  class OutputDeviceControl;
  class SmtcBridge;
  class TrackListController;
  class TrackPropertiesCoordinator;
  class ThemeCoordinator;
} // namespace ao::winui

namespace ao::winui::layout
{
  class ShellBuilder;
}

namespace winrt::Aobus::implementation
{
  // Generated WinRT/XAML virtuals implement the platform ABI and are validated by the native MSVC build.
  // NOLINTNEXTLINE(portability-template-virtual-member-function)
  struct MainWindow : MainWindowT<MainWindow>
  {
    /// What the frame will answer to. Every value here is one some caller asks about.
    enum class SessionPhase : std::uint8_t
    {
      Constructed,
      Prepared,
      Active,
      Retired,
    };

    using RestartLibraryCallback = ao::compat::MoveOnlyFunction<ao::Result<>(std::filesystem::path)>;

    MainWindow();
    ~MainWindow() override;

    MainWindow(MainWindow const&) = delete;
    MainWindow& operator=(MainWindow const&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    void initialize(ao::winui::LibrarySession& session, RestartLibraryCallback requestRestart);
    ao::Result<> activate();
    ao::Result<> prepareLibraryRestart();
    void retire() noexcept;

    void handleRootSizeChanged(Windows::Foundation::IInspectable const&,
                               Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
    void handleRootPointerPressed(Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void handleNavigateBackInvoked(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                                   Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
    void handleNavigateForwardInvoked(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                                      Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
    void handleTrackPropertiesInvoked(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                                      Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
    void handleColumnHeaderClicked(Windows::Foundation::IInspectable const& sender,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&);
    void handleColumnResizeCompleted(Windows::Foundation::IInspectable const& sender,
                                     Microsoft::UI::Xaml::Controls::Primitives::DragCompletedEventArgs const& args);
    void handleColumnMoveLeftClicked(Windows::Foundation::IInspectable const& sender,
                                     Microsoft::UI::Xaml::RoutedEventArgs const&);
    void handleColumnMoveRightClicked(Windows::Foundation::IInspectable const& sender,
                                      Microsoft::UI::Xaml::RoutedEventArgs const&);
    void handleGroupCoverLoaded(Windows::Foundation::IInspectable const& sender,
                                Microsoft::UI::Xaml::RoutedEventArgs const&);
    void handleGroupCoverDataContextChanged(Microsoft::UI::Xaml::FrameworkElement const& sender,
                                            Microsoft::UI::Xaml::DataContextChangedEventArgs const&);
    void handleGroupCoverUnloaded(Windows::Foundation::IInspectable const& sender,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&);

  private:
    struct GroupCoverPresenterEntry final
    {
      weak_ref<Microsoft::UI::Xaml::Controls::Grid> tile;
      std::unique_ptr<ao::winui::CoverArtPresenter> presenterPtr;
    };

    void shutdown() noexcept;
    /// Stand up the document-built shell and hand it the frame's own commands.
    void createShellBuilder();
    void reconcileLibrary();
    void refreshGroupCoverPresenter(Windows::Foundation::IInspectable const& sender);
    void clearGroupCoverPresenters() noexcept;
    void unbindPlayback() noexcept;
    void bindPlayback();
    bool tryApplyShellState(double width, std::optional<ao::winui::ShellMode> optCandidateMode = std::nullopt);
    void restoreWindowPlacement();
    void saveWindowState() noexcept;
    void applyTheme(ao::winui::Theme const& theme);
    void applySystemTheme();
    void rebuildForTheme();
    void updateStatus(std::string const& status);
    ao::utility::ScopedRegistration subscribeAppWindowChanges(Microsoft::UI::Windowing::AppWindow window,
                                                              void (MainWindow::*updateActivity)());
    void updateSoulWindowActivity();
    void updateFullscreenSoulWindowActivity();
    void showFullscreenSoul();
    void showSystemMenu();
    bool isModalWorkflowActive() const noexcept;
    void navigateHistory(bool forward);

    /**
     * @name Frame commands
     *
     * What a shell asks the window to do. Named rather than reached through
     * event handlers, because the document-built shell invokes them from an
     * action registry that knows nothing about XAML events.
     * @{
     */
    void rescanLibrary();
    void importLibrary();
    void exportLibrary();
    void playPause();
    void stopPlayback();
    void revealCurrentTrack();
    void presentTrackProperties();
    void presentCapturedTrackProperties(std::vector<ao::TrackId> trackIds,
                                        std::optional<std::bitset<ao::library::kCreditKindCount>> optScope);
    void toggleInspector();
    void toggleShellMode();
    void reloadTheme();
    void showColumnsMenu();
    void showOutputDeviceSelector(Microsoft::UI::Xaml::FrameworkElement const& anchor);
    /// @}

    void executeSort(std::string const& columnId);
    void moveColumn(Windows::Foundation::IInspectable const& sender, std::int32_t offset);
    fire_and_forget pickLibrary();

    ao::winui::LibrarySession* _session = nullptr;
    RestartLibraryCallback _requestRestart;
    /**
     * @name Window-scoped runtime consumers
     *
     * Built in `initialize` from the borrowed session and released in
     * `shutdown`. Declared ahead of every generation and adapter that borrows
     * them, so fallback destruction cannot outlive them; `shutdown` still
     * retires them in an explicit order, because a reference-counted XAML graph
     * is not quiesced by member order alone.
     * @{
     */
    std::unique_ptr<ao::winui::TrackListController> _trackListPtr;
    std::unique_ptr<ao::winui::ThemeCoordinator> _themePtr;
    /// Routes the runtime's reveal requests to this window's track list.
    ao::async::Subscription _revealTrackSub;
    /// @}
    /// The one shell there is; null only before `initialize` and after `shutdown`.
    std::unique_ptr<ao::winui::layout::ShellBuilder> _shellBuilderPtr;
    /// The selector a document's soul or output button raises, which no generation owns.
    std::unique_ptr<ao::winui::OutputDeviceControl> _shellOutputDevicePtr;
    /// At most one revision-bound authoring dialog belongs to the window.
    std::unique_ptr<ao::winui::TrackPropertiesCoordinator> _trackPropertiesCoordinatorPtr;
    /// Window-lifetime owner of native List, Playlist-membership, and order workflows.
    std::unique_ptr<ao::winui::ListAuthoringCoordinator> _listAuthoringCoordinatorPtr;
    /// Window-lifetime owner of native YAML transfer dialogs, pickers, and tasks.
    std::unique_ptr<ao::winui::LibraryTransferCoordinator> _libraryTransferCoordinatorPtr;
    /// The loaded theme override, or nothing while Windows' own appearance is in force.
    std::optional<ao::winui::Theme> _optThemeOverride;
    std::unique_ptr<ao::winui::SmtcBridge> _smtcPtr;
    /// The two transport commands a preset's menu can name but no menu item can drive.
    std::unique_ptr<ao::uimodel::TransportViewModel> _playPausePtr;
    std::unique_ptr<ao::uimodel::TransportViewModel> _stopPtr;
    Microsoft::UI::Xaml::Window _soulWindow{nullptr};
    // Keep the projected handle distinct from the same-named implementation class in every include order.
    ::winrt::Aobus::AobusSoulControl _fullscreenSoul{nullptr};
    ao::utility::ScopedRegistration _appWindowChangedSub;
    ao::utility::ScopedRegistration _soulWindowChangedSub;
    /**
     * @brief What the user last asked of the inspector, if anything.
     *
     * Empty until they ask, which is what lets each presentation start where it
     * belongs: inline showing, overlay hidden. Held by the frame rather than by
     * a generation so a mode or theme rebuild does not answer for the user, and
     * deliberately not persisted: it is a reveal, not a setting.
     */
    std::optional<bool> _optInspectorRequest;
    std::unordered_map<void const*, GroupCoverPresenterEntry> _groupCoverPresenters;
    SessionPhase _sessionPhase = SessionPhase::Constructed;
  };
} // namespace winrt::Aobus::implementation

namespace winrt::Aobus::factory_implementation
{
  // Generated C++/WinRT factory virtuals implement the ABI and are validated by the native MSVC build.
  // NOLINTNEXTLINE(portability-template-virtual-member-function)
  struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
  {};
}

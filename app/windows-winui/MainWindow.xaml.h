// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "MainWindow.g.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ao::uimodel
{
  class NowPlayingViewModel;
  struct NowPlayingViewState;
  struct WindowsTheme;
}

namespace ao::winui
{
  class AudioPipelineToolTip;
  class CoverArtPresenter;
  class LibrarySession;
  class PlaybackControls;
  class SmtcBridge;
  class TrackListController;
  class WindowsUiCoordinator;
  class WindowsThemeCoordinator;
}

namespace winrt::Aobus::implementation
{
  struct MainWindow : MainWindowT<MainWindow>
  {
    MainWindow();
    ~MainWindow();

    void initialize(ao::winui::LibrarySession& session);
    void retire();

    void OnRootSizeChanged(Windows::Foundation::IInspectable const&,
                           Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
    void OnOpenLibraryClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnRescanClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnToggleModeClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnReloadThemeClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnPlayPauseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnStopClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnClassicSoulHolding(Windows::Foundation::IInspectable const&,
                              Microsoft::UI::Xaml::Input::HoldingRoutedEventArgs const& args);
    void OnClassicSoulRightTapped(Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const& args);
    void OnSystemMenuClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnNavigationSelectionChanged(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args);
    void OnClassicTreeSelectionChanged(Microsoft::UI::Xaml::Controls::TreeView const&,
                                       Microsoft::UI::Xaml::Controls::TreeViewSelectionChangedEventArgs const&);
    void OnTrackSelectionChanged(Windows::Foundation::IInspectable const& sender,
                                 Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnTrackDoubleTapped(Windows::Foundation::IInspectable const& sender,
                             Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
    void OnTrackViewportSizeChanged(Windows::Foundation::IInspectable const&,
                                    Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
    void OnPaneResizeDelta(Windows::Foundation::IInspectable const& sender,
                           Microsoft::UI::Xaml::Controls::Primitives::DragDeltaEventArgs const& args);
    void OnPaneResizeCompleted(Windows::Foundation::IInspectable const&,
                               Microsoft::UI::Xaml::Controls::Primitives::DragCompletedEventArgs const&);
    void OnColumnHeaderClicked(Windows::Foundation::IInspectable const& sender,
                               Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnColumnResizeCompleted(Windows::Foundation::IInspectable const& sender,
                                 Microsoft::UI::Xaml::Controls::Primitives::DragCompletedEventArgs const& args);
    void OnColumnMoveLeftClicked(Windows::Foundation::IInspectable const& sender,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnColumnMoveRightClicked(Windows::Foundation::IInspectable const& sender,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnColumnsClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnPresentationClicked(Windows::Foundation::IInspectable const& sender,
                               Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnInspectorToggleClicked(Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&);

  private:
    struct NavigationEntry final
    {
      ao::ListId listId = ao::kInvalidListId;
      std::string label{};
    };

    void shutdown();
    void reconcileLibrary();
    void rebuildNavigation();
    void navigateTo(NavigationEntry const& entry);
    void updateBrowserHeader();
    void updateTrackSurfaceWidth();
    void unbindPlayback();
    void bindPlayback();
    void applyShellState(double width);
    void restoreWindowPlacement();
    void saveWindowState();
    void applyTheme(ao::uimodel::WindowsTheme const& theme);
    void applySystemTheme();
    void updateStatus(std::string const& status);
    void updateNowPlaying(ao::uimodel::NowPlayingViewState const& state);
    void updateSoulWindowActivity();
    void updateFullscreenSoulWindowActivity();
    void showFullscreenSoul();
    void showSystemMenu();
    void executeSort(std::string const& columnId);
    void moveColumn(Windows::Foundation::IInspectable const& sender, int offset);
    winrt::fire_and_forget pickLibrary();

    ao::winui::LibrarySession* _session = nullptr;
    std::unique_ptr<ao::winui::WindowsUiCoordinator> _coordinatorPtr;
    ao::winui::TrackListController* _trackListPtr = nullptr;
    ao::winui::CoverArtPresenter* _nowPlayingCoverArtPtr = nullptr;
    std::unique_ptr<ao::winui::SmtcBridge> _smtcPtr;
    ao::winui::WindowsThemeCoordinator* _themePtr = nullptr;
    std::unique_ptr<ao::winui::PlaybackControls> _playbackControlsPtr;
    std::unique_ptr<ao::winui::AudioPipelineToolTip> _audioPipelineToolTipPtr;
    std::unique_ptr<ao::uimodel::NowPlayingViewModel> _nowPlayingPtr;
    Microsoft::UI::Xaml::Window _soulWindow{nullptr};
    Aobus::AobusSoulControl _fullscreenSoul{nullptr};
    event_token _appWindowChangedToken{};
    event_token _closedToken{};
    event_token _soulWindowChangedToken{};
    bool _inspectorRequested = false;
    bool _applyingNavigation = false;
    bool _applyingTrackSelection = false;
    std::map<ao::ListId, NavigationEntry> _navigationEntriesById;
    bool _hasAppWindowChangedToken = false;
    bool _hasClosedToken = false;
    bool _hasSoulWindowChangedToken = false;
    bool _paneResizeDirty = false;
    bool _shutdown = false;
  };
}

namespace winrt::Aobus::factory_implementation
{
  struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
  {};
}

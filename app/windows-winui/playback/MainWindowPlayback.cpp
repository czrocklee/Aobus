// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "app/WindowsUiCoordinator.h"
#include "image/CoverArtPresenter.h"
#include "pch.h"
#include "platform/SmtcBridge.h"
#include "platform/WindowsStringResources.h"
#include "playback/AobusSoulControl.h"
#include "playback/AudioPipelineToolTip.h"
#include "playback/PlaybackControls.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include <memory>

namespace winrt::Aobus::implementation
{
  namespace
  {
    constexpr double kFullscreenSoulPadding = 48.0;

    HWND nativeWindow(Microsoft::UI::Xaml::Window const& window)
    {
      auto windowNative = window.as<::IWindowNative>();
      HWND handle = nullptr;
      check_hresult(windowNative->get_WindowHandle(&handle));
      return handle;
    }

    bool isMinimized(Microsoft::UI::Windowing::AppWindow const& window)
    {
      if (auto presenter = window.Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>(); presenter)
      {
        return presenter.State() == Microsoft::UI::Windowing::OverlappedPresenterState::Minimized;
      }

      return false;
    }
  } // namespace

  void MainWindow::unbindPlayback()
  {
    _smtcPtr.reset();
    _nowPlayingPtr.reset();

    if (_playbackControlsPtr)
    {
      _playbackControlsPtr->unbind();
    }

    get_self<AobusSoulControl>(ClassicSoul())->unbind();

    if (_fullscreenSoul)
    {
      get_self<AobusSoulControl>(_fullscreenSoul)->unbind();
    }
  }

  void MainWindow::bindPlayback()
  {
    if (_session == nullptr || !_coordinatorPtr)
    {
      return;
    }

    unbindPlayback();
    auto const dependencies = _coordinatorPtr->uiDependencies();
    auto& playback = dependencies.runtime.playback();
    auto& commands = dependencies.playbackCommands;
    _playbackControlsPtr->bind(dependencies);
    _nowPlayingPtr = std::make_unique<ao::uimodel::NowPlayingViewModel>(
      playback, [this](ao::uimodel::NowPlayingViewState const& state) { updateNowPlaying(state); });

    get_self<AobusSoulControl>(ClassicSoul())->bind(playback);
    get_self<AobusSoulControl>(ClassicSoul())->setTransportIcon(ao::uimodel::TransportIcon::None);

    if (_fullscreenSoul)
    {
      get_self<AobusSoulControl>(_fullscreenSoul)->bind(playback);
    }

    try
    {
      _smtcPtr = std::make_unique<ao::winui::SmtcBridge>(
        nativeWindow(*this), Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread());
      _smtcPtr->bind(_session->runtimePtr(), commands, dependencies.resourceBytes);
    }
    catch (hresult_error const& error)
    {
      updateStatus(ao::winui::formatResource("SmtcUnavailableFormat", to_string(error.message())));
    }
  }

  void MainWindow::updateSoulWindowActivity()
  {
    auto const window = AppWindow();
    auto const visible = window.IsVisible();
    auto const minimized = isMinimized(window);
    get_self<AobusSoulControl>(ModernSoul())->setWindowActivity(visible, minimized);
    get_self<AobusSoulControl>(ClassicSoul())->setWindowActivity(visible, minimized);
  }

  void MainWindow::updateFullscreenSoulWindowActivity()
  {
    if (!_soulWindow || !_fullscreenSoul)
    {
      return;
    }

    auto const window = _soulWindow.AppWindow();
    get_self<AobusSoulControl>(_fullscreenSoul)->setWindowActivity(window.IsVisible(), isMinimized(window));
  }

  void MainWindow::OnPlayPauseClicked(Windows::Foundation::IInspectable const& /*sender*/,
                                      Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    if (_playbackControlsPtr)
    {
      _playbackControlsPtr->activatePlayPause();
    }
  }

  void MainWindow::OnStopClicked(Windows::Foundation::IInspectable const& /*sender*/,
                                 Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    if (_playbackControlsPtr)
    {
      _playbackControlsPtr->activateStop();
    }
  }

  void MainWindow::updateNowPlaying(ao::uimodel::NowPlayingViewState const& state)
  {
    ModernNowPlayingTitle().Text(state.isActive ? to_hstring(state.title) : ao::winui::resourceHstring(L"NotPlaying"));
    ModernNowPlayingArtist().Text(state.artist == "Unknown Artist" ? ao::winui::resourceHstring(L"UnknownArtist")
                                                                   : to_hstring(state.artist));

    if (_nowPlayingCoverArtPtr != nullptr)
    {
      _nowPlayingCoverArtPtr->select(state.coverArtId, state.coverArtPlaceholderIdentity, true);
    }

    if (_audioPipelineToolTipPtr)
    {
      _audioPipelineToolTipPtr->apply(state.audioPipeline);
    }
  }

  void MainWindow::OnClassicSoulHolding(Windows::Foundation::IInspectable const& /*sender*/,
                                        Microsoft::UI::Xaml::Input::HoldingRoutedEventArgs const& args)
  {
    if (args.HoldingState() == Microsoft::UI::Input::HoldingState::Completed)
    {
      showFullscreenSoul();
      args.Handled(true);
    }
  }

  void MainWindow::showFullscreenSoul()
  {
    if (_soulWindow)
    {
      _soulWindow.Activate();
      return;
    }

    _soulWindow = Microsoft::UI::Xaml::Window{};
    _soulWindow.Title(ao::winui::resourceHstring(L"SoulWindowTitle"));
    _fullscreenSoul = Aobus::AobusSoulControl{};
    get_self<AobusSoulControl>(_fullscreenSoul)->setTransportIcon(ao::uimodel::TransportIcon::None);

    if (_session != nullptr)
    {
      get_self<AobusSoulControl>(_fullscreenSoul)->bind(_session->runtime().playback());
    }

    auto root = Microsoft::UI::Xaml::Controls::Grid{};
    root.Padding({
      .Left = kFullscreenSoulPadding,
      .Top = kFullscreenSoulPadding,
      .Right = kFullscreenSoulPadding,
      .Bottom = kFullscreenSoulPadding,
    });
    root.Children().Append(_fullscreenSoul);
    _soulWindow.Content(root);
    auto weak = get_weak();
    _soulWindowChangedToken = _soulWindow.AppWindow().Changed(
      [weak](
        Microsoft::UI::Windowing::AppWindow const&, Microsoft::UI::Windowing::AppWindowChangedEventArgs const& args)
      {
        if ((args.DidVisibilityChange() || args.DidPresenterChange()))
        {
          if (auto self = weak.get(); self)
          {
            self->updateFullscreenSoulWindowActivity();
          }
        }
      });
    _hasSoulWindowChangedToken = true;
    _soulWindow.Closed(
      [weak](Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::WindowEventArgs const&)
      {
        if (auto self = weak.get(); self)
        {
          if (self->_hasSoulWindowChangedToken && self->_soulWindow)
          {
            self->_soulWindow.AppWindow().Changed(self->_soulWindowChangedToken);
            self->_hasSoulWindowChangedToken = false;
          }

          if (self->_fullscreenSoul)
          {
            get_self<AobusSoulControl>(self->_fullscreenSoul)->unbind();
          }

          self->_fullscreenSoul = nullptr;
          self->_soulWindow = nullptr;
        }
      });

    if (auto presenter = _soulWindow.AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>();
        presenter)
    {
      presenter.Maximize();
    }

    _soulWindow.Activate();
    updateFullscreenSoulWindowActivity();
  }

  void MainWindow::OnClassicSoulRightTapped(Windows::Foundation::IInspectable const& /*sender*/,
                                            Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const& args)
  {
    showSystemMenu();
    args.Handled(true);
  }

  void MainWindow::OnSystemMenuClicked(Windows::Foundation::IInspectable const& /*sender*/,
                                       Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    showSystemMenu();
  }

  void MainWindow::showSystemMenu()
  {
    auto* const handle = nativeWindow(*this);
    ::SetForegroundWindow(handle);
    ::PostMessageW(handle, WM_SYSCOMMAND, SC_KEYMENU, static_cast<LPARAM>(L' '));
  }
} // namespace winrt::Aobus::implementation

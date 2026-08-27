// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "layout/ShellBuilder.h"
#include "pch.h"
#include "platform/SmtcBridge.h"
#include "platform/StringResources.h"
#include "playback/AobusSoulControl.h"
#include "playback/OutputDeviceControl.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
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

  void MainWindow::unbindPlayback() noexcept
  {
    _smtcPtr.reset();
    _playPausePtr.reset();
    _stopPtr.reset();

    if (_shellOutputDevicePtr)
    {
      _shellOutputDevicePtr->unbind();
    }

    if (_fullscreenSoul)
    {
      get_self<AobusSoulControl>(_fullscreenSoul)->unbind();
    }
  }

  void MainWindow::bindPlayback()
  {
    if (_session == nullptr || !_resourceBytesPtr)
    {
      return;
    }

    unbindPlayback();
    auto& runtime = _session->runtime();
    auto& playback = runtime.playback();
    auto& commands = _session->playbackCommands();

    if (_shellOutputDevicePtr)
    {
      _shellOutputDevicePtr->bind(playback);
    }

    // The two commands a menu can name have no button of their own in a
    // document, so the frame keeps a view model for each rather than reaching
    // into whichever transport the live generation happens to have built.
    _playPausePtr = std::make_unique<ao::uimodel::TransportViewModel>(
      playback, commands, _session->textCatalog(), ao::uimodel::PlaybackCommand::PlayPause, false, [](auto const&) {});
    _stopPtr = std::make_unique<ao::uimodel::TransportViewModel>(
      playback, commands, _session->textCatalog(), ao::uimodel::PlaybackCommand::Stop, false, [](auto const&) {});

    if (_fullscreenSoul)
    {
      get_self<AobusSoulControl>(_fullscreenSoul)->bind(playback);
    }

    try
    {
      _smtcPtr =
        std::make_unique<ao::winui::SmtcBridge>(nativeWindow(*this),
                                                Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread(),
                                                runtime,
                                                commands,
                                                *_resourceBytesPtr);
    }
    catch (hresult_error const& error)
    {
      updateStatus(ao::winui::formatResource("winui_smtc_unavailable", to_string(error.message())));
    }
  }

  void MainWindow::updateSoulWindowActivity()
  {
    // Whether the window can be seen is the frame's to know; which souls that
    // reaches is the live generation's.
    if (auto const window = AppWindow(); _shellBuilderPtr)
    {
      _shellBuilderPtr->applyWindowActivity(window.IsVisible(), isMinimized(window));
    }
  }

  void MainWindow::playPause()
  {
    if (_playPausePtr)
    {
      _playPausePtr->handleClick();
    }
  }

  void MainWindow::stopPlayback()
  {
    if (_stopPtr)
    {
      _stopPtr->handleClick();
    }
  }

  void MainWindow::showOutputDeviceSelector(Microsoft::UI::Xaml::FrameworkElement const& anchor)
  {
    if (_shellOutputDevicePtr)
    {
      _shellOutputDevicePtr->showAt(anchor);
    }
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
    _soulWindowChangedSub =
      subscribeAppWindowChanges(_soulWindow.AppWindow(), &MainWindow::updateFullscreenSoulWindowActivity);
    _soulWindow.Closed(
      [weak](Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::WindowEventArgs const&)
      {
        if (auto self = weak.get(); self)
        {
          self->_soulWindowChangedSub.reset();

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

  void MainWindow::showSystemMenu()
  {
    auto* const handle = nativeWindow(*this);
    ::SetForegroundWindow(handle);
    ::PostMessageW(handle, WM_SYSCOMMAND, SC_KEYMENU, static_cast<LPARAM>(L' '));
  }
} // namespace winrt::Aobus::implementation

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// Native COM declarations must precede the C++/WinRT headers for IWindowNative interop.
#include <microsoft.ui.xaml.window.h>
#include <unknwn.h>
#include <windows.h>

// The SDK macro conflicts with XAML's GetCurrentTime method.
#undef GetCurrentTime

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "layout/ShellBuilder.h"
#include "platform/AppWindowActivity.h"
#include "platform/SmtcBridge.h"
#include "platform/StringResourceFormat.h"
#include "playback/FullscreenSoulWindow.h"
#include "playback/OutputDeviceControl.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/base.h>

#include <memory>

namespace winrt::Aobus::implementation
{
  namespace
  {
    HWND nativeWindow(Microsoft::UI::Xaml::Window const& window)
    {
      auto windowNative = window.as<::IWindowNative>();
      HWND handle = nullptr;
      check_hresult(windowNative->get_WindowHandle(&handle));
      return handle;
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
  }

  void MainWindow::bindPlayback()
  {
    if (_session == nullptr)
    {
      return;
    }

    unbindPlayback();
    auto& runtime = _session->runtime();
    auto& playback = runtime.playback();
    auto& commands = _session->playbackActions();

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

    try
    {
      _smtcPtr =
        std::make_unique<ao::winui::SmtcBridge>(nativeWindow(*this),
                                                Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread(),
                                                runtime.playback(),
                                                commands,
                                                runtime.resourceBytes(),
                                                runtime.async());
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
      _shellBuilderPtr->applyWindowActivity(window.IsVisible(), ao::winui::isWindowMinimized(window));
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

  void MainWindow::showFullscreenSoul()
  {
    if (_fullscreenSoulWindowPtr)
    {
      _fullscreenSoulWindowPtr->show();
    }
  }

  void MainWindow::showSystemMenu()
  {
    auto* const handle = nativeWindow(*this);
    ::SetForegroundWindow(handle);
    ::PostMessageW(handle, WM_SYSCOMMAND, SC_KEYMENU, static_cast<LPARAM>(L' '));
  }
} // namespace winrt::Aobus::implementation

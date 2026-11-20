// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/MainWindowPlayback.h"

#include "platform/SmtcBridge.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <winrt/Microsoft.UI.Dispatching.h>

#include <memory>

namespace ao::winui
{
  MainWindowPlayback::MainWindowPlayback() = default;
  MainWindowPlayback::~MainWindowPlayback()
  {
    unbind();
  }

  void MainWindowPlayback::bindTransport(rt::PlaybackService& playback,
                                         uimodel::PlaybackActions& actions,
                                         i18n::MessageCatalog const& textCatalog)
  {
    // A menu command has no button of its own. Keep its view model independent of the live shell generation.
    _playPausePtr = std::make_unique<uimodel::TransportViewModel>(
      playback, actions, textCatalog, uimodel::PlaybackCommand::PlayPause, false, [](auto const&) {});
    _stopPtr = std::make_unique<uimodel::TransportViewModel>(
      playback, actions, textCatalog, uimodel::PlaybackCommand::Stop, false, [](auto const&) {});
  }

  void MainWindowPlayback::bindSystemMediaControls(HWND window,
                                                   rt::PlaybackService& playback,
                                                   uimodel::PlaybackActions& actions,
                                                   rt::ResourceByteMemoryCache& resourceBytes,
                                                   async::Runtime& asyncRuntime)
  {
    _smtcPtr = std::make_unique<SmtcBridge>(window,
                                            winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread(),
                                            playback,
                                            actions,
                                            resourceBytes,
                                            asyncRuntime);
  }

  void MainWindowPlayback::unbind() noexcept
  {
    _smtcPtr.reset();
    _playPausePtr.reset();
    _stopPtr.reset();
  }

  void MainWindowPlayback::playPause()
  {
    if (_playPausePtr)
    {
      _playPausePtr->handleClick();
    }
  }

  void MainWindowPlayback::stop()
  {
    if (_stopPtr)
    {
      _stopPtr->handleClick();
    }
  }
} // namespace ao::winui

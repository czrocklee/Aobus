// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Use the SDK entry point for HWND without leaking the macro that collides with XAML.
#pragma push_macro("GetCurrentTime")
#include <windows.h>
#pragma pop_macro("GetCurrentTime")

#include <memory>

namespace ao::async
{
  class Runtime;
}

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  class PlaybackService;
  class ResourceByteMemoryCache;
}

namespace ao::uimodel
{
  class PlaybackActions;
  class TransportViewModel;
}

namespace ao::winui
{
  class SmtcBridge;

  /// Playback commands that survive shell rebuilds, and the window's optional system media controls.
  class MainWindowPlayback final
  {
  public:
    MainWindowPlayback();
    ~MainWindowPlayback();

    MainWindowPlayback(MainWindowPlayback const&) = delete;
    MainWindowPlayback& operator=(MainWindowPlayback const&) = delete;
    MainWindowPlayback(MainWindowPlayback&&) = delete;
    MainWindowPlayback& operator=(MainWindowPlayback&&) = delete;

    void bindTransport(rt::PlaybackService& playback,
                       uimodel::PlaybackActions& actions,
                       i18n::MessageCatalog const& textCatalog);
    void bindSystemMediaControls(HWND window,
                                 rt::PlaybackService& playback,
                                 uimodel::PlaybackActions& actions,
                                 rt::ResourceByteMemoryCache& resourceBytes,
                                 async::Runtime& asyncRuntime);
    void unbind() noexcept;
    void playPause();
    void stop();

  private:
    std::unique_ptr<SmtcBridge> _smtcPtr;
    std::unique_ptr<uimodel::TransportViewModel> _playPausePtr;
    std::unique_ptr<uimodel::TransportViewModel> _stopPtr;
  };
} // namespace ao::winui

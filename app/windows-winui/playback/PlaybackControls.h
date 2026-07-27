// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <memory>

namespace ao::winui
{
  struct WinUiDependencies;

  struct ModernPlaybackControlsConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Button shuffleButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button previousButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button outputButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button soulButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ContentControl soul{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button nextButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button repeatButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Slider seek{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock elapsed{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock duration{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Slider volume{nullptr};
  };

  struct ClassicPlaybackControlsConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Button previousButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button nextButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button soulButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button playPauseButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button stopButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Slider seek{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock time{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Slider volume{nullptr};
  };

  struct PlaybackControlsConfig final
  {
    ModernPlaybackControlsConfig modern;
    ClassicPlaybackControlsConfig classic;
  };

  class PlaybackControls final
  {
  public:
    explicit PlaybackControls(PlaybackControlsConfig config);
    ~PlaybackControls();

    PlaybackControls(PlaybackControls const&) = delete;
    PlaybackControls& operator=(PlaybackControls const&) = delete;
    PlaybackControls(PlaybackControls&&) = delete;
    PlaybackControls& operator=(PlaybackControls&&) = delete;

    void bind(WinUiDependencies const& dependencies);
    void unbind();
    void setPresentationActive(bool modern);
    void activatePlayPause();
    void activateStop();

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::winui

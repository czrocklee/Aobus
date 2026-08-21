// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <memory>

namespace ao::uimodel
{
  struct AudioPipelineViewState;
  class NowPlayingViewModel;
} // namespace ao::uimodel

namespace ao::rt
{
  class PlaybackService;
} // namespace ao::rt

namespace ao::winui
{
  struct AudioPipelineToolTipConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Button anchor{nullptr};
    uimodel::PresentationTextCatalog textCatalog;
  };

  /**
   * @brief The audio pipeline explanation attached to one anchor.
   *
   * Every shell hangs this on the soul button, so the tooltip follows the
   * playback state itself rather than being fed by whichever surface happens to
   * observe it: one anchor, one subscription, no coordination.
   */
  class AudioPipelineToolTip final
  {
  public:
    explicit AudioPipelineToolTip(AudioPipelineToolTipConfig config);
    ~AudioPipelineToolTip();

    AudioPipelineToolTip(AudioPipelineToolTip const&) = delete;
    AudioPipelineToolTip& operator=(AudioPipelineToolTip const&) = delete;
    AudioPipelineToolTip(AudioPipelineToolTip&&) = delete;
    AudioPipelineToolTip& operator=(AudioPipelineToolTip&&) = delete;

    void bind(rt::PlaybackService& playback);
    void unbind() noexcept;

  private:
    /// Blank the widget between bindings. Only a rebind has anything to show.
    void resetPresentation();

    void apply(uimodel::AudioPipelineViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::Button _anchor{nullptr};
    uimodel::PresentationTextCatalog _textCatalog;
    winrt::Microsoft::UI::Xaml::Controls::ToolTip _toolTip{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _text{nullptr};
    std::unique_ptr<uimodel::NowPlayingViewModel> _viewModelPtr;
  };
} // namespace ao::winui

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace ao::uimodel
{
  struct AudioPipelineViewState;
}

namespace ao::winui
{
  struct AudioPipelineToolTipConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Button modernAnchor{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button classicAnchor{nullptr};
  };

  class AudioPipelineToolTip final
  {
  public:
    explicit AudioPipelineToolTip(AudioPipelineToolTipConfig config);
    ~AudioPipelineToolTip();

    AudioPipelineToolTip(AudioPipelineToolTip const&) = delete;
    AudioPipelineToolTip& operator=(AudioPipelineToolTip const&) = delete;
    AudioPipelineToolTip(AudioPipelineToolTip&&) = delete;
    AudioPipelineToolTip& operator=(AudioPipelineToolTip&&) = delete;

    void apply(uimodel::AudioPipelineViewState const& state);

  private:
    struct Presenter final
    {
      winrt::Microsoft::UI::Xaml::Controls::Button anchor{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::ToolTip toolTip{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::TextBlock text{nullptr};
    };

    static Presenter makePresenter(winrt::Microsoft::UI::Xaml::Controls::Button anchor);
    static void apply(Presenter const& presenter, uimodel::AudioPipelineViewState const& state);
    static void detach(Presenter const& presenter);

    Presenter _modern;
    Presenter _classic;
  };
} // namespace ao::winui

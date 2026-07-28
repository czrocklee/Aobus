// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioPipelineToolTip.h"

#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include <utility>

namespace ao::winui
{
  namespace
  {
    constexpr double kMaximumWidth = 360.0;
  }

  AudioPipelineToolTip::AudioPipelineToolTip(AudioPipelineToolTipConfig config)
    : _modern{makePresenter(std::move(config.modernAnchor))}, _classic{makePresenter(std::move(config.classicAnchor))}
  {
  }

  AudioPipelineToolTip::~AudioPipelineToolTip()
  {
    detach(_modern);
    detach(_classic);
  }

  void AudioPipelineToolTip::apply(uimodel::AudioPipelineViewState const& state)
  {
    apply(_modern, state);
    apply(_classic, state);
  }

  AudioPipelineToolTip::Presenter AudioPipelineToolTip::makePresenter(
    winrt::Microsoft::UI::Xaml::Controls::Button anchor)
  {
    using winrt::Microsoft::UI::Xaml::TextWrapping;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTip;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;
    using winrt::Microsoft::UI::Xaml::Controls::Primitives::PlacementMode;

    auto text = TextBlock{};
    text.MaxWidth(kMaximumWidth);
    text.TextWrapping(TextWrapping::Wrap);

    auto toolTip = ToolTip{};
    toolTip.Content(text);
    toolTip.Placement(PlacementMode::Top);
    toolTip.IsEnabled(false);
    ToolTipService::SetToolTip(anchor, toolTip);

    return Presenter{
      .anchor = std::move(anchor),
      .toolTip = std::move(toolTip),
      .text = std::move(text),
    };
  }

  void AudioPipelineToolTip::apply(Presenter const& presenter, uimodel::AudioPipelineViewState const& state)
  {
    using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;

    auto const text = winrt::to_hstring(state.plainTextFallback);
    auto const available = !state.quality.assessments.empty() && !text.empty();
    presenter.text.Text(text);
    presenter.toolTip.IsEnabled(available);
    AutomationProperties::SetHelpText(presenter.anchor, available ? text : winrt::hstring{});
  }

  void AudioPipelineToolTip::detach(Presenter const& presenter)
  {
    using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;

    if (!presenter.anchor)
    {
      return;
    }

    presenter.toolTip.IsOpen(false);
    ToolTipService::SetToolTip(presenter.anchor, winrt::Windows::Foundation::IInspectable{nullptr});
    AutomationProperties::SetHelpText(presenter.anchor, winrt::hstring{});
  }
} // namespace ao::winui

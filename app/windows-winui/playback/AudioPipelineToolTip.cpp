// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioPipelineToolTip.h"

#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include <memory>
#include <utility>

namespace ao::winui
{
  namespace
  {
    constexpr double kMaximumWidth = 360.0;
  }

  AudioPipelineToolTip::AudioPipelineToolTip(AudioPipelineToolTipConfig config, ao::rt::PlaybackService& playback)
    : _anchor{std::move(config.anchor)}, _textCatalog{std::move(config.textCatalog)}
  {
    using winrt::Microsoft::UI::Xaml::TextWrapping;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTip;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;
    using winrt::Microsoft::UI::Xaml::Controls::Primitives::PlacementMode;

    _text = TextBlock{};
    _text.MaxWidth(kMaximumWidth);
    _text.TextWrapping(TextWrapping::Wrap);

    _toolTip = ToolTip{};
    _toolTip.Content(_text);
    _toolTip.Placement(PlacementMode::Top);
    _toolTip.IsEnabled(false);
    ToolTipService::SetToolTip(_anchor, _toolTip);

    resetPresentation();
    _viewModelPtr = std::make_unique<uimodel::NowPlayingViewModel>(
      playback, _textCatalog, [this](uimodel::NowPlayingViewState const& state) { apply(state.audioPipeline); });
  }

  AudioPipelineToolTip::~AudioPipelineToolTip()
  {
    _viewModelPtr.reset();

    if (!_anchor)
    {
      return;
    }

    using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;

    _toolTip.IsOpen(false);
    ToolTipService::SetToolTip(_anchor, winrt::Windows::Foundation::IInspectable{nullptr});
    AutomationProperties::SetHelpText(_anchor, winrt::hstring{});
  }

  void AudioPipelineToolTip::resetPresentation()
  {
    apply({});
  }

  void AudioPipelineToolTip::apply(uimodel::AudioPipelineViewState const& state)
  {
    using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;

    if (!_anchor)
    {
      return;
    }

    auto const text = winrt::to_hstring(state.plainTextFallback);
    auto const available = !state.quality.assessments.empty() && !text.empty();
    _text.Text(text);
    _toolTip.IsEnabled(available);
    AutomationProperties::SetHelpText(_anchor, available ? text : winrt::hstring{});
  }
} // namespace ao::winui

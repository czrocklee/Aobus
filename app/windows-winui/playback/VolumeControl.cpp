// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/VolumeControl.h"

#include "platform/ScopedBooleanFlag.h"
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/output/VolumeViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <memory>
#include <utility>

namespace ao::winui
{
  VolumeControl::VolumeControl(VolumeControlConfig config, ao::rt::PlaybackService& playback)
    : _slider{std::move(config.slider)}, _textCatalog{std::move(config.textCatalog)}
  {
    _valueChangedRevoker = _slider.ValueChanged(
      winrt::auto_revoke,
      [this](winrt::Windows::Foundation::IInspectable const&,
             winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
      {
        if (!_updating && _viewModelPtr)
        {
          _viewModelPtr->handleVolumeChanged(static_cast<float>(args.NewValue()));
        }
      });
    resetPresentation();
    _viewModelPtr = std::make_unique<uimodel::VolumeViewModel>(
      playback, _textCatalog, [this](uimodel::VolumeViewState const& state) { applyState(state); });
  }

  VolumeControl::~VolumeControl() = default;

  void VolumeControl::resetPresentation()
  {
    if (_slider)
    {
      _slider.IsEnabled(false);
      winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(_slider, nullptr);
      winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(_slider, L"");
    }
  }

  void VolumeControl::applyState(uimodel::VolumeViewState const& state)
  {
    [[maybe_unused]] auto const updating = ScopedBooleanFlag{_updating};
    _slider.Value(state.volume);
    _slider.Visibility(state.visible ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                     : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    _slider.IsEnabled(state.visible);
    auto const tooltip = winrt::to_hstring(state.tooltip);
    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(_slider, winrt::box_value(tooltip));
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(_slider, tooltip);
  }
} // namespace ao::winui

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/VolumeControl.h"

#include "platform/ScopedBooleanFlag.h"
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/output/VolumeViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <memory>
#include <utility>

namespace ao::winui
{
  VolumeControl::VolumeControl(VolumeControlConfig config)
    : _slider{std::move(config.slider)}
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
  }

  VolumeControl::~VolumeControl()
  {
    unbind();
  }

  void VolumeControl::bind(ao::rt::PlaybackService& playback)
  {
    unbind();
    resetPresentation();
    _viewModelPtr = std::make_unique<uimodel::VolumeViewModel>(
      playback, [this](uimodel::VolumeViewState const& state) { applyState(state); });
  }

  void VolumeControl::unbind() noexcept
  {
    _viewModelPtr.reset();
  }

  void VolumeControl::resetPresentation()
  {
    if (_slider)
    {
      _slider.IsEnabled(false);
    }
  }

  void VolumeControl::applyState(uimodel::VolumeViewState const& state)
  {
    [[maybe_unused]] auto const updating = ScopedBooleanFlag{_updating};
    _slider.Value(state.volume);
    _slider.Visibility(state.visible ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                     : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    _slider.IsEnabled(state.visible);
  }
} // namespace ao::winui

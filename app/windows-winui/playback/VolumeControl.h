// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/output/VolumeViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <memory>

namespace ao::rt
{
  class PlaybackService;
} // namespace ao::rt

namespace ao::winui
{
  struct VolumeControlConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Slider slider{nullptr};
  };

  class VolumeControl final
  {
  public:
    explicit VolumeControl(VolumeControlConfig config);
    ~VolumeControl();

    VolumeControl(VolumeControl const&) = delete;
    VolumeControl& operator=(VolumeControl const&) = delete;
    VolumeControl(VolumeControl&&) = delete;
    VolumeControl& operator=(VolumeControl&&) = delete;

    void bind(rt::PlaybackService& playback);
    void unbind() noexcept;

  private:
    /// Blank the widget between bindings. Only a rebind has anything to show.
    void resetPresentation();

    void applyState(uimodel::VolumeViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::Slider _slider{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Slider::ValueChanged_revoker _valueChangedRevoker{};
    std::unique_ptr<uimodel::VolumeViewModel> _viewModelPtr;
    bool _updating = false;
  };
} // namespace ao::winui

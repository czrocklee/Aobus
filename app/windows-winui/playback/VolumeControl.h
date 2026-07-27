// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/output/VolumeViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <memory>

namespace ao::winui
{
  struct WinUiDependencies;

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

    void bind(WinUiDependencies const& dependencies);
    void unbind();

  private:
    void applyState(uimodel::VolumeViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::Slider _slider{nullptr};
    winrt::event_token _valueChangedToken{};
    std::unique_ptr<uimodel::VolumeViewModel> _viewModelPtr;
    bool _updating = false;
  };
} // namespace ao::winui

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
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
    i18n::MessageCatalog textCatalog;
  };

  class VolumeControl final
  {
  public:
    VolumeControl(VolumeControlConfig config, rt::PlaybackService& playback);
    ~VolumeControl();

    VolumeControl(VolumeControl const&) = delete;
    VolumeControl& operator=(VolumeControl const&) = delete;
    VolumeControl(VolumeControl&&) = delete;
    VolumeControl& operator=(VolumeControl&&) = delete;

  private:
    /// Establish a blank state before the first model snapshot.
    void resetPresentation();

    void applyState(uimodel::VolumeViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::Slider _slider{nullptr};
    i18n::MessageCatalog _textCatalog;
    winrt::Microsoft::UI::Xaml::Controls::Slider::ValueChanged_revoker _valueChangedRevoker{};
    std::unique_ptr<uimodel::VolumeViewModel> _viewModelPtr;
    bool _updating = false;
  };
} // namespace ao::winui

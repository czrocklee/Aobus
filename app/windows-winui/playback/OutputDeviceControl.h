// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <memory>
#include <vector>

namespace ao::winui
{
  struct WinUiDependencies;

  struct OutputDeviceControlConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Button modernButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button classicButton{nullptr};
  };

  class OutputDeviceControl final
  {
  public:
    explicit OutputDeviceControl(OutputDeviceControlConfig config);
    ~OutputDeviceControl();

    OutputDeviceControl(OutputDeviceControl const&) = delete;
    OutputDeviceControl& operator=(OutputDeviceControl const&) = delete;
    OutputDeviceControl(OutputDeviceControl&&) = delete;
    OutputDeviceControl& operator=(OutputDeviceControl&&) = delete;

    void bind(WinUiDependencies const& dependencies);
    void unbind();

  private:
    struct ItemClickRegistration final
    {
      winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem item{nullptr};
      winrt::event_token token{};
    };

    void applyState(uimodel::OutputDeviceViewState const& state);
    void showAt(winrt::Microsoft::UI::Xaml::Controls::Button const& anchor);
    void closeFlyout();

    winrt::Microsoft::UI::Xaml::Controls::Button _modernButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _classicButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout _flyout{nullptr};
    winrt::event_token _modernClickToken{};
    winrt::event_token _classicClickToken{};
    std::vector<ItemClickRegistration> _itemClickRegistrations;
    std::unique_ptr<uimodel::OutputDeviceViewModel> _viewModelPtr;
    uimodel::OutputDeviceViewState _state{};
  };
} // namespace ao::winui

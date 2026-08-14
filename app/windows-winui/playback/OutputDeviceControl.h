// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/OutputDeviceSelection.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <functional>
#include <memory>
#include <vector>

namespace ao::rt
{
  class PlaybackService;
} // namespace ao::rt

namespace ao::winui
{
  struct OutputDeviceControlConfig final
  {
    /**
     * @brief The button that names the active output, if any surface presents it.
     *
     * The selector is also reachable from anchors that present nothing of their
     * own, such as the soul button, so the presenter is optional and the control
     * is then only a way to raise the menu.
     */
    winrt::Microsoft::UI::Xaml::Controls::Button presenter{nullptr};
    std::function<void(audio::OutputDeviceSelection const&)> onSelectionRequested{};
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

    void bind(rt::PlaybackService& playback);
    void unbind() noexcept;

    /// Present the device menu anchored to @p anchor, refreshed from the runtime.
    void showAt(winrt::Microsoft::UI::Xaml::FrameworkElement const& anchor);

  private:
    /// Blank the widget between bindings. Only a rebind has anything to show.
    void resetPresentation();

    void applyState(uimodel::OutputDeviceViewState const& state);
    void closeFlyout() noexcept;

    winrt::Microsoft::UI::Xaml::Controls::Button _presenter{nullptr};
    std::function<void(audio::OutputDeviceSelection const&)> _onSelectionRequested;
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout _flyout{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _presenterClickRevoker{};
    std::vector<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem::Click_revoker> _itemClickRevokers;
    std::unique_ptr<uimodel::OutputDeviceViewModel> _viewModelPtr;
    uimodel::OutputDeviceViewState _state{};
  };
} // namespace ao::winui

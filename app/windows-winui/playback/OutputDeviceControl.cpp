// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputDeviceControl.h"

#include "app/WinUiDependencies.h"
#include "platform/WindowsStringResources.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <format>
#include <memory>
#include <utility>

namespace ao::winui
{
  OutputDeviceControl::OutputDeviceControl(OutputDeviceControlConfig config)
    : _modernButton{std::move(config.modernButton)}, _classicButton{std::move(config.classicButton)}
  {
    _modernClickToken =
      _modernButton.Click([this](winrt::Windows::Foundation::IInspectable const&,
                                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) { showAt(_modernButton); });
    _classicClickToken =
      _classicButton.Click([this](winrt::Windows::Foundation::IInspectable const&,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) { showAt(_classicButton); });
  }

  OutputDeviceControl::~OutputDeviceControl()
  {
    unbind();

    if (_modernButton)
    {
      _modernButton.Click(_modernClickToken);
    }

    if (_classicButton)
    {
      _classicButton.Click(_classicClickToken);
    }
  }

  void OutputDeviceControl::bind(WinUiDependencies const& dependencies)
  {
    unbind();
    _viewModelPtr = std::make_unique<uimodel::OutputDeviceViewModel>(dependencies.playbackRuntime.playback(),
                                                                     [this](uimodel::OutputDeviceViewState const& state)
                                                                     { applyState(state); });
    _viewModelPtr->refresh();
  }

  void OutputDeviceControl::unbind()
  {
    closeFlyout();
    _viewModelPtr.reset();
    _state = {};

    if (_modernButton)
    {
      _modernButton.Content(winrt::box_value(L"--"));
      winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
        _modernButton, winrt::box_value(resourceHstring(L"OutputDeviceTooltip")));
    }
  }

  void OutputDeviceControl::applyState(uimodel::OutputDeviceViewState const& state)
  {
    _state = state;
    _modernButton.Content(winrt::box_value(winrt::to_hstring(state.outputBackendSummary)));
    auto tooltip = winrt::Windows::Foundation::IInspectable{winrt::box_value(resourceHstring(L"OutputDeviceTooltip"))};

    if (!state.outputDeviceStatus.empty())
    {
      tooltip = winrt::box_value(winrt::to_hstring(state.outputDeviceStatus));
    }

    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(_modernButton, tooltip);
  }

  void OutputDeviceControl::showAt(winrt::Microsoft::UI::Xaml::Controls::Button const& anchor)
  {
    if (!_viewModelPtr)
    {
      return;
    }

    _viewModelPtr->refresh();
    closeFlyout();
    _flyout = winrt::Microsoft::UI::Xaml::Controls::MenuFlyout{};

    for (auto const& row : _state.rows)
    {
      auto item = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem{};
      item.Text(winrt::to_hstring(row.isActive ? std::format("✓ {}", row.title) : row.title));

      if (row.kind == uimodel::OutputDeviceRow::Kind::BackendHeader)
      {
        item.IsEnabled(false);
      }
      else
      {
        auto const token = item.Click(
          [this, backendId = row.backendId, deviceId = row.deviceId, profileId = row.profileId](
            winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
          {
            if (_viewModelPtr)
            {
              _viewModelPtr->selectOutputDevice(backendId, deviceId, profileId);
            }
          });
        _itemClickRegistrations.push_back(ItemClickRegistration{.item = item, .token = token});
      }

      _flyout.Items().Append(item);
    }

    if (_state.rows.empty())
    {
      auto item = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem{};
      item.Text(resourceHstring(L"NoOutputDevices"));
      item.IsEnabled(false);
      _flyout.Items().Append(item);
    }

    _flyout.ShowAt(anchor);
  }

  void OutputDeviceControl::closeFlyout()
  {
    for (auto const& registration : _itemClickRegistrations)
    {
      if (registration.item)
      {
        registration.item.Click(registration.token);
      }
    }

    _itemClickRegistrations.clear();

    if (_flyout)
    {
      _flyout.Hide();
      _flyout = nullptr;
    }
  }
} // namespace ao::winui

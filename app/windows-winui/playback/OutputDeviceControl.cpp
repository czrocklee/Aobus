// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputDeviceControl.h"

#include "platform/StringResources.h"
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <format>
#include <memory>
#include <utility>

namespace ao::winui
{
  OutputDeviceControl::OutputDeviceControl(OutputDeviceControlConfig config)
    : _presenter{std::move(config.presenter)}
  {
    if (_presenter)
    {
      _presenterClickRevoker = _presenter.Click(
        winrt::auto_revoke,
        [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        { showAt(_presenter); });
    }
  }

  OutputDeviceControl::~OutputDeviceControl()
  {
    unbind();
  }

  void OutputDeviceControl::bind(ao::rt::PlaybackService& playback)
  {
    unbind();
    resetPresentation();
    _viewModelPtr = std::make_unique<uimodel::OutputDeviceViewModel>(
      playback, [this](uimodel::OutputDeviceViewState const& state) { applyState(state); });
    _viewModelPtr->refresh();
  }

  void OutputDeviceControl::unbind() noexcept
  {
    _viewModelPtr.reset();
    closeFlyout();
    _state = {};
  }

  void OutputDeviceControl::resetPresentation()
  {
    if (_presenter)
    {
      _presenter.Content(winrt::box_value(L"--"));
      winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
        _presenter, winrt::box_value(resourceHstring(L"OutputDeviceTooltip")));
    }
  }

  void OutputDeviceControl::applyState(uimodel::OutputDeviceViewState const& state)
  {
    _state = state;

    if (!_presenter)
    {
      return;
    }

    _presenter.Content(winrt::box_value(winrt::to_hstring(state.outputBackendSummary)));
    auto tooltip = winrt::Windows::Foundation::IInspectable{winrt::box_value(resourceHstring(L"OutputDeviceTooltip"))};

    if (!state.outputDeviceStatus.empty())
    {
      tooltip = winrt::box_value(winrt::to_hstring(state.outputDeviceStatus));
    }

    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(_presenter, tooltip);
  }

  void OutputDeviceControl::showAt(winrt::Microsoft::UI::Xaml::FrameworkElement const& anchor)
  {
    if (!_viewModelPtr || !anchor)
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
        _itemClickRevokers.push_back(item.Click(
          winrt::auto_revoke,
          [this, backendId = row.backendId, deviceId = row.deviceId, profileId = row.profileId](
            winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
          {
            if (_viewModelPtr)
            {
              _viewModelPtr->selectOutputDevice(backendId, deviceId, profileId);
            }
          }));
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

  void OutputDeviceControl::closeFlyout() noexcept
  {
    _itemClickRevokers.clear();

    if (_flyout)
    {
      try
      {
        _flyout.Hide();
      }
      // NOLINTNEXTLINE(bugprone-empty-catch): Hiding a projected flyout cannot block its owner release.
      catch (...)
      {
      }

      _flyout = nullptr;
    }
  }
} // namespace ao::winui

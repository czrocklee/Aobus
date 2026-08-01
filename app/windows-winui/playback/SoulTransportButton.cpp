// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SoulTransportButton.h"

#include "app/WinUiDependencies.h"
#include "platform/WindowsStringResources.h"
#include "playback/AobusSoulControl.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <memory>
#include <utility>

namespace ao::winui
{
  SoulTransportButton::SoulTransportButton(SoulTransportButtonConfig config)
    : _button{std::move(config.button)}, _soul{std::move(config.soul)}, _hasComplexTooltip{config.hasComplexTooltip}
  {
    _clickToken = _button.Click([this](winrt::Windows::Foundation::IInspectable const&,
                                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) { activate(); });
  }

  SoulTransportButton::~SoulTransportButton()
  {
    unbind();

    if (_button)
    {
      _button.Click(_clickToken);
    }
  }

  void SoulTransportButton::bind(WinUiDependencies const& dependencies)
  {
    unbind();
    auto& playback = dependencies.runtime.playback();
    auto soul = _soul.as<winrt::Aobus::AobusSoulControl>();
    winrt::get_self<winrt::Aobus::implementation::AobusSoulControl>(soul)->bind(playback);
    _viewModelPtr = std::make_unique<uimodel::TransportViewModel>(playback,
                                                                  dependencies.playbackCommands,
                                                                  uimodel::PlaybackCommand::PlayPause,
                                                                  false,
                                                                  [this](uimodel::TransportViewState const& state)
                                                                  { applyState(state); });
  }

  void SoulTransportButton::unbind()
  {
    _viewModelPtr.reset();

    if (_soul)
    {
      auto soul = _soul.as<winrt::Aobus::AobusSoulControl>();
      winrt::get_self<winrt::Aobus::implementation::AobusSoulControl>(soul)->unbind();
    }

    if (_button)
    {
      _button.IsEnabled(false);
    }
  }

  void SoulTransportButton::activate()
  {
    if (_viewModelPtr)
    {
      _viewModelPtr->handleClick();
    }
  }

  void SoulTransportButton::applyState(uimodel::TransportViewState const& state)
  {
    _button.IsEnabled(state.enabled);

    if (!_hasComplexTooltip)
    {
      winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
        _button,
        winrt::box_value(
          resourceHstring(state.icon == uimodel::TransportIcon::Pause ? L"PauseTooltip" : L"PlayTooltip")));
    }

    auto soul = _soul.as<winrt::Aobus::AobusSoulControl>();
    winrt::get_self<winrt::Aobus::implementation::AobusSoulControl>(soul)->setTransportIcon(state.icon);
  }
} // namespace ao::winui

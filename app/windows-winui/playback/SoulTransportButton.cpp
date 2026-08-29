// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SoulTransportButton.h"

#include "playback/AobusSoulControl.h"
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <memory>
#include <utility>

namespace ao::winui
{
  SoulTransportButton::SoulTransportButton(SoulTransportButtonConfig config,
                                           ao::rt::PlaybackService& playback,
                                           ao::uimodel::PlaybackActions& actions)
    : _button{std::move(config.button)}
    , _soul{std::move(config.soul)}
    , _textCatalog{std::move(config.textCatalog)}
    , _hasComplexTooltip{config.hasComplexTooltip}
    , _showGlyph{config.showGlyph}
    , _activatesOnClick{config.activatesOnClick}
  {
    if (_activatesOnClick)
    {
      _clickRevoker = _button.Click(winrt::auto_revoke,
                                    [this](winrt::Windows::Foundation::IInspectable const&,
                                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) { activate(); });
    }

    resetPresentation();
    auto soul = _soul.as<winrt::Aobus::AobusSoulControl>();
    winrt::get_self<winrt::Aobus::implementation::AobusSoulControl>(soul)->bind(playback);
    _viewModelPtr = std::make_unique<uimodel::TransportViewModel>(playback,
                                                                  actions,
                                                                  _textCatalog,
                                                                  uimodel::PlaybackCommand::PlayPause,
                                                                  false,
                                                                  [this](uimodel::TransportViewState const& state)
                                                                  { applyState(state); });
  }

  SoulTransportButton::~SoulTransportButton()
  {
    stop();
  }

  void SoulTransportButton::stop() noexcept
  {
    _viewModelPtr.reset();

    if (_soul)
    {
      auto soul = _soul.as<winrt::Aobus::AobusSoulControl>();
      winrt::get_self<winrt::Aobus::implementation::AobusSoulControl>(soul)->unbind();
    }
  }

  void SoulTransportButton::resetPresentation()
  {
    // Enablement follows the transport only while the transport owns the click:
    // a button whose gesture the shell claimed must stay usable regardless.
    if (_button && _activatesOnClick)
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
    if (_activatesOnClick)
    {
      _button.IsEnabled(state.enabled);

      if (!_hasComplexTooltip)
      {
        winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
          _button, winrt::box_value(winrt::to_hstring(state.tooltip)));
      }
    }

    auto soul = _soul.as<winrt::Aobus::AobusSoulControl>();
    winrt::get_self<winrt::Aobus::implementation::AobusSoulControl>(soul)->setTransportIcon(
      _showGlyph ? state.icon : uimodel::TransportIcon::None);
  }
} // namespace ao::winui

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TransportButton.h"

#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <memory>
#include <utility>

namespace ao::winui
{
  namespace
  {
    winrt::Microsoft::UI::Xaml::Controls::Symbol symbolForTransport(uimodel::TransportIcon const icon) noexcept
    {
      using Icon = uimodel::TransportIcon;
      using Symbol = winrt::Microsoft::UI::Xaml::Controls::Symbol;

      switch (icon)
      {
        case Icon::Pause: return Symbol::Pause;
        case Icon::Stop: return Symbol::Stop;
        case Icon::Next: return Symbol::Next;
        case Icon::Previous: return Symbol::Previous;
        case Icon::Shuffle: return Symbol::Shuffle;
        case Icon::Repeat:
        case Icon::RepeatOne: return Symbol::RepeatAll;
        case Icon::Play:
        case Icon::None: return Symbol::Play;
      }

      return Symbol::Play;
    }
  } // namespace

  TransportButton::TransportButton(TransportButtonConfig config)
    : _button{std::move(config.button)}
    , _textCatalog{std::move(config.textCatalog)}
    , _command{config.command}
    , _showLabel{config.showLabel}
  {
    _clickRevoker = _button.Click(winrt::auto_revoke,
                                  [this](winrt::Windows::Foundation::IInspectable const&,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) { activate(); });
  }

  TransportButton::~TransportButton()
  {
    unbind();
  }

  void TransportButton::bind(ao::rt::PlaybackService& playback, ao::uimodel::PlaybackCommandSurface& commands)
  {
    unbind();
    resetPresentation();
    _viewModelPtr = std::make_unique<uimodel::TransportViewModel>(playback,
                                                                  commands,
                                                                  _textCatalog,
                                                                  _command,
                                                                  _showLabel,
                                                                  [this](uimodel::TransportViewState const& state)
                                                                  { applyState(state); });
  }

  void TransportButton::unbind() noexcept
  {
    _viewModelPtr.reset();
  }

  void TransportButton::resetPresentation()
  {
    if (_button)
    {
      _button.IsEnabled(false);
    }
  }

  void TransportButton::activate()
  {
    if (_viewModelPtr)
    {
      _viewModelPtr->handleClick();
    }
  }

  void TransportButton::applyState(uimodel::TransportViewState const& state)
  {
    _button.IsEnabled(state.enabled);
    _button.Content(winrt::Microsoft::UI::Xaml::Controls::SymbolIcon{symbolForTransport(state.icon)});

    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
      _button, winrt::box_value(winrt::to_hstring(state.tooltip)));
  }
} // namespace ao::winui

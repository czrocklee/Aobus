// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TransportButton.h"

#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
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
        case Icon::Repeat: return Symbol::RepeatAll;
        case Icon::RepeatOne: return Symbol::RepeatOne;
        case Icon::Play:
        case Icon::None: return Symbol::Play;
      }

      return Symbol::Play;
    }
  } // namespace

  TransportButton::TransportButton(TransportButtonConfig config,
                                   ao::rt::PlaybackService& playback,
                                   ao::uimodel::PlaybackActions& actions)
    : _button{std::move(config.button)}, _engagedForeground{std::move(config.engagedForeground)}
  {
    _clickRevoker = _button.Click(winrt::auto_revoke,
                                  [this](winrt::Windows::Foundation::IInspectable const&,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) { activate(); });
    resetPresentation();
    _viewModelPtr = std::make_unique<uimodel::TransportViewModel>(playback,
                                                                  actions,
                                                                  std::move(config.textCatalog),
                                                                  config.command,
                                                                  /*showLabel=*/false,
                                                                  [this](uimodel::TransportViewState const& state)
                                                                  { applyState(state); });
  }

  TransportButton::~TransportButton() = default;

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

    // An engaged command is the one glyph the transport recolors, and the frame
    // resolves which brush that is once per generation. Releasing the local
    // value, rather than setting null, returns the foreground to the theme and
    // any authored `styleKey`, which is also how a frame that publishes no
    // accent brush degrades.
    if (state.engaged && _engagedForeground)
    {
      _button.Foreground(_engagedForeground);
    }
    else
    {
      _button.ClearValue(winrt::Microsoft::UI::Xaml::Controls::Control::ForegroundProperty());
    }

    auto const tooltip = winrt::to_hstring(state.tooltip);
    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(_button, winrt::box_value(tooltip));
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(_button, tooltip);
  }
} // namespace ao::winui

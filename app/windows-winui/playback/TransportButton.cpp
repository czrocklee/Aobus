// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TransportButton.h"

#include "app/WinUiDependencies.h"
#include "platform/WindowsStringResources.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <string_view>
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

    std::wstring_view tooltipResource(uimodel::TransportIcon const icon) noexcept
    {
      using Icon = uimodel::TransportIcon;

      switch (icon)
      {
        case Icon::Play: return L"PlayTooltip";
        case Icon::Pause: return L"PauseTooltip";
        case Icon::Stop: return L"StopTooltip";
        case Icon::Next: return L"NextTooltip";
        case Icon::Previous: return L"PreviousTooltip";
        case Icon::Shuffle: return L"ShuffleTooltip";
        case Icon::Repeat:
        case Icon::RepeatOne: return L"RepeatTooltip";
        case Icon::None: return {};
      }

      return {};
    }
  } // namespace

  TransportButton::TransportButton(TransportButtonConfig config)
    : _button{std::move(config.button)}, _command{config.command}, _showLabel{config.showLabel}
  {
    _clickToken = _button.Click([this](winrt::Windows::Foundation::IInspectable const&,
                                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) { activate(); });
  }

  TransportButton::~TransportButton()
  {
    unbind();

    if (_button)
    {
      _button.Click(_clickToken);
    }
  }

  void TransportButton::bind(WinUiDependencies const& dependencies)
  {
    unbind();
    _viewModelPtr = std::make_unique<uimodel::TransportViewModel>(dependencies.playbackRuntime.playback(),
                                                                  dependencies.playbackCommands,
                                                                  _command,
                                                                  _showLabel,
                                                                  [this](uimodel::TransportViewState const& state)
                                                                  { applyState(state); });
  }

  void TransportButton::unbind()
  {
    _viewModelPtr.reset();

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

    auto const resourceId = tooltipResource(state.icon);
    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
      _button, winrt::box_value(resourceId.empty() ? winrt::to_hstring(state.tooltip) : resourceHstring(resourceId)));
  }
} // namespace ao::winui

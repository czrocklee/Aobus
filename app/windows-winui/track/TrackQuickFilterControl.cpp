// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackQuickFilterControl.h"

#include "platform/ScopedBooleanFlag.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/library/track/TrackFilterViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

namespace ao::winui
{
  namespace
  {
    constexpr auto kFilterDebounceInterval = std::chrono::milliseconds{200};
  } // namespace

  TrackQuickFilterControl::TrackQuickFilterControl(TrackQuickFilterControlConfig config)
    : _input{std::move(config.input)}
    , _debounceTimer{_input.DispatcherQueue().CreateTimer()}
    , _onError{std::move(config.onError)}
  {
    _debounceTimer.Interval(kFilterDebounceInterval);
    _debounceTimer.IsRepeating(false);
    _textChangedRevoker =
      _input.TextChanged(winrt::auto_revoke,
                         [this](winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const&,
                                winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const& args)
                         { handleTextChanged(args); });
    _debounceTickRevoker =
      _debounceTimer.Tick(winrt::auto_revoke,
                          [this](winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const&,
                                 winrt::Windows::Foundation::IInspectable const&) { commitPendingText(); });
  }

  TrackQuickFilterControl::~TrackQuickFilterControl()
  {
    _commitPending = false;

    if (_debounceTimer)
    {
      _debounceTimer.Stop();
    }

    _viewModelPtr.reset();
    _runtimePtr.reset();
  }

  void TrackQuickFilterControl::bind(std::shared_ptr<rt::AppRuntime> runtimePtr)
  {
    unbind();
    _runtimePtr = std::move(runtimePtr);

    try
    {
      _viewModelPtr = std::make_unique<uimodel::TrackFilterViewModel>(_runtimePtr->views(),
                                                                      _runtimePtr->workspace(),
                                                                      [this](uimodel::TrackFilterViewState const& state)
                                                                      { applyState(state); });
    }
    catch (...)
    {
      _runtimePtr.reset();
      throw;
    }
  }

  void TrackQuickFilterControl::unbind()
  {
    _commitPending = false;

    if (_debounceTimer)
    {
      _debounceTimer.Stop();
    }

    _viewModelPtr.reset();
    _runtimePtr.reset();

    if (_input)
    {
      [[maybe_unused]] auto const applyingState = ScopedBooleanFlag{_applyingState};
      _input.IsEnabled(false);
      _input.Text(L"");
      winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(_input, nullptr);
    }
  }

  void TrackQuickFilterControl::handleTextChanged(
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const& args)
  {
    if (_applyingState || !_viewModelPtr ||
        args.Reason() != winrt::Microsoft::UI::Xaml::Controls::AutoSuggestionBoxTextChangeReason::UserInput)
    {
      return;
    }

    _debounceTimer.Stop();
    _debounceTimer.Interval(kFilterDebounceInterval);
    _commitDeadline = std::chrono::steady_clock::now() + kFilterDebounceInterval;
    _commitPending = true;
    _debounceTimer.Start();
  }

  void TrackQuickFilterControl::commitPendingText()
  {
    if (!_commitPending)
    {
      return;
    }

    if (auto const now = std::chrono::steady_clock::now(); now < _commitDeadline)
    {
      auto const remaining =
        std::max(std::chrono::milliseconds{1}, std::chrono::ceil<std::chrono::milliseconds>(_commitDeadline - now));
      _debounceTimer.Stop();
      _debounceTimer.Interval(remaining);
      _debounceTimer.Start();
      return;
    }

    _commitPending = false;

    if (_viewModelPtr)
    {
      _viewModelPtr->updateFilter(winrt::to_string(_input.Text()));
    }
  }

  void TrackQuickFilterControl::applyState(uimodel::TrackFilterViewState const& state)
  {
    _commitPending = false;
    _debounceTimer.Stop();
    {
      [[maybe_unused]] auto const applyingState = ScopedBooleanFlag{_applyingState};
      _input.IsEnabled(state.enabled);

      if (winrt::to_string(_input.Text()) != state.entryText)
      {
        _input.Text(winrt::to_hstring(state.entryText));
      }
    }

    auto tooltip = winrt::Windows::Foundation::IInspectable{nullptr};

    if (!state.tooltip.empty())
    {
      tooltip = winrt::box_value(winrt::to_hstring(state.tooltip));
    }

    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(_input, tooltip);

    if (state.hasError && _onError)
    {
      _onError(state.tooltip);
    }
  }
} // namespace ao::winui

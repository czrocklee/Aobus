// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::uimodel
{
  class TrackFilterViewModel;
  struct TrackFilterViewState;
}

namespace ao::winui
{
  struct TrackQuickFilterControlConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox input{nullptr};
    std::function<void(std::string)> onError;
  };

  class TrackQuickFilterControl final
  {
  public:
    explicit TrackQuickFilterControl(TrackQuickFilterControlConfig config);
    ~TrackQuickFilterControl();

    TrackQuickFilterControl(TrackQuickFilterControl const&) = delete;
    TrackQuickFilterControl& operator=(TrackQuickFilterControl const&) = delete;
    TrackQuickFilterControl(TrackQuickFilterControl&&) = delete;
    TrackQuickFilterControl& operator=(TrackQuickFilterControl&&) = delete;

    void bind(std::shared_ptr<rt::AppRuntime> runtimePtr);
    void unbind();

  private:
    void handleTextChanged(winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const& args);
    void commitPendingText();
    void applyState(uimodel::TrackFilterViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox _input{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _debounceTimer{nullptr};
    std::function<void(std::string)> _onError;
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::TextChanged_revoker _textChangedRevoker{};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer::Tick_revoker _debounceTickRevoker{};
    std::shared_ptr<rt::AppRuntime> _runtimePtr;
    std::unique_ptr<uimodel::TrackFilterViewModel> _viewModelPtr;
    std::chrono::steady_clock::time_point _commitDeadline{};
    bool _applyingState = false;
    bool _commitPending = false;
  };
} // namespace ao::winui

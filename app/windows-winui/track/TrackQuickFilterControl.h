// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/winui/track/QuickFilterCompletionAdapter.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  class CompletionService;
  class ViewService;
  class WorkspaceService;
}

namespace ao::uimodel
{
  class TrackFilterCompleter;
  class TrackFilterViewModel;
  struct TrackFilterViewState;
}

namespace ao::winui
{
  struct TrackQuickFilterControlConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox input{nullptr};
    std::function<void(std::string)> onError;
    std::function<void(uimodel::TrackFilterViewState const&)> onState;
    i18n::MessageCatalog textCatalog;
  };

  class TrackQuickFilterControl final
  {
  public:
    TrackQuickFilterControl(TrackQuickFilterControlConfig config,
                            rt::ViewService& views,
                            rt::WorkspaceService& workspace,
                            rt::CompletionService& completion);
    ~TrackQuickFilterControl();

    TrackQuickFilterControl(TrackQuickFilterControl const&) = delete;
    TrackQuickFilterControl& operator=(TrackQuickFilterControl const&) = delete;
    TrackQuickFilterControl(TrackQuickFilterControl&&) = delete;
    TrackQuickFilterControl& operator=(TrackQuickFilterControl&&) = delete;

  private:
    /// Establish a blank state before the first model snapshot.
    void resetPresentation();
    void stop() noexcept;

    void handleTextChanged(winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const& args);
    void handleSuggestionChosen(
      winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxSuggestionChosenEventArgs const& args);
    void handleQuerySubmitted(winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxQuerySubmittedEventArgs const& args);
    void handlePreviewKeyDown(winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void handleLoaded();
    bool bindEditor();
    void refreshSuggestions();
    void clearSuggestions();
    void forgetSuggestions() noexcept;
    std::optional<std::size_t> suggestionIndex(winrt::Windows::Foundation::IInspectable const& suggestion) const;
    bool acceptSuggestion(std::size_t index);
    void schedulePendingText();
    void commitCurrentText();
    void commitPendingText();
    void applyState(uimodel::TrackFilterViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox _input{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _debounceTimer{nullptr};
    std::function<void(std::string)> _onError;
    std::function<void(uimodel::TrackFilterViewState const&)> _onState;
    i18n::MessageCatalog _textCatalog;
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable>
      _suggestionItems{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox _editor{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::TextChanged_revoker _textChangedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::SuggestionChosen_revoker _suggestionChosenRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::QuerySubmitted_revoker _querySubmittedRevoker{};
    winrt::Microsoft::UI::Xaml::FrameworkElement::Loaded_revoker _loadedRevoker{};
    winrt::Microsoft::UI::Xaml::UIElement::PreviewKeyDown_revoker _previewKeyDownRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::TextBox::SelectionChanged_revoker _selectionChangedRevoker{};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer::Tick_revoker _debounceTickRevoker{};
    std::unique_ptr<uimodel::TrackFilterViewModel> _viewModelPtr;
    std::unique_ptr<uimodel::TrackFilterCompleter> _completerPtr;
    std::optional<rt::CompletionResult> _optCompletionResult;
    std::vector<QuickFilterSuggestionRow> _suggestionRows;
    std::string _completionDraft;
    std::optional<std::size_t> _optPendingSuggestionIndex;
    std::chrono::steady_clock::time_point _commitDeadline{};
    bool _applyingState = false;
    bool _commitPending = false;
  };
} // namespace ao::winui

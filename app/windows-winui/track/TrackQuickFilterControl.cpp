// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackQuickFilterControl.h"

#include "platform/ScopedBooleanFlag.h"
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/uimodel/library/track/TrackFilter.h>
#include <ao/uimodel/library/track/TrackFilterView.h>
#include <ao/winui/track/QuickFilterCompletionAdapter.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ao::winui
{
  namespace
  {
    constexpr auto kFilterDebounceInterval = std::chrono::milliseconds{200};
    constexpr double kSuggestionDetailFontSize = 12.0;
    constexpr double kSuggestionDetailOpacity = 0.68;

    winrt::Microsoft::UI::Xaml::Controls::TextBox findTextBox(winrt::Microsoft::UI::Xaml::DependencyObject const& root)
    {
      using winrt::Microsoft::UI::Xaml::Controls::TextBox;
      using winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper;

      if (!root)
      {
        return nullptr;
      }

      if (auto editor = root.try_as<TextBox>(); editor)
      {
        return editor;
      }

      auto const childCount = VisualTreeHelper::GetChildrenCount(root);

      for (std::int32_t index = 0; index < childCount; ++index)
      {
        if (auto editor = findTextBox(VisualTreeHelper::GetChild(root, index)); editor)
        {
          return editor;
        }
      }

      return nullptr;
    }

    winrt::Microsoft::UI::Xaml::FrameworkElement makeSuggestionElement(QuickFilterSuggestionRow const& row,
                                                                       std::uint32_t const index)
    {
      using namespace winrt::Microsoft::UI::Xaml;
      using namespace winrt::Microsoft::UI::Xaml::Controls;

      auto panel = StackPanel{};
      panel.Tag(winrt::box_value(index));

      auto primary = TextBlock{};
      primary.Text(winrt::to_hstring(row.displayText));
      primary.TextTrimming(TextTrimming::CharacterEllipsis);
      panel.Children().Append(primary);

      if (!row.detailText.empty())
      {
        auto detail = TextBlock{};
        detail.Text(winrt::to_hstring(row.detailText));
        detail.FontSize(kSuggestionDetailFontSize);
        detail.Opacity(kSuggestionDetailOpacity);
        detail.TextTrimming(TextTrimming::CharacterEllipsis);
        panel.Children().Append(detail);
      }

      return panel;
    }
  } // namespace

  TrackQuickFilterControl::TrackQuickFilterControl(TrackQuickFilterControlConfig config)
    : _input{std::move(config.input)}
    , _debounceTimer{_input.DispatcherQueue().CreateTimer()}
    , _onError{std::move(config.onError)}
    , _onState{std::move(config.onState)}
    , _textCatalog{std::move(config.textCatalog)}
    , _suggestionItems{winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>()}
  {
    _input.UpdateTextOnSelect(false);
    _input.ItemsSource(_suggestionItems);
    _debounceTimer.Interval(kFilterDebounceInterval);
    _debounceTimer.IsRepeating(false);
    _textChangedRevoker =
      _input.TextChanged(winrt::auto_revoke,
                         [this](winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const&,
                                winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const& args)
                         { handleTextChanged(args); });
    _suggestionChosenRevoker = _input.SuggestionChosen(
      winrt::auto_revoke,
      [this](winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const&,
             winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxSuggestionChosenEventArgs const& args)
      { handleSuggestionChosen(args); });
    _querySubmittedRevoker = _input.QuerySubmitted(
      winrt::auto_revoke,
      [this](winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const&,
             winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxQuerySubmittedEventArgs const& args)
      { handleQuerySubmitted(args); });
    _loadedRevoker = _input.Loaded(winrt::auto_revoke,
                                   [this](winrt::Windows::Foundation::IInspectable const&,
                                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) { handleLoaded(); });
    _previewKeyDownRevoker = _input.PreviewKeyDown(
      winrt::auto_revoke,
      [this](winrt::Windows::Foundation::IInspectable const&,
             winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) { handlePreviewKeyDown(args); });
    _debounceTickRevoker =
      _debounceTimer.Tick(winrt::auto_revoke,
                          [this](winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const&,
                                 winrt::Windows::Foundation::IInspectable const&) { commitPendingText(); });
  }

  TrackQuickFilterControl::~TrackQuickFilterControl()
  {
    unbind();
  }

  void TrackQuickFilterControl::bind(rt::ViewService& views,
                                     rt::WorkspaceService& workspace,
                                     rt::CompletionService& completion)
  {
    unbind();
    resetPresentation();
    _completerPtr = std::make_unique<uimodel::TrackFilterCompleter>(completion);
    _viewModelPtr = std::make_unique<uimodel::TrackFilterViewModel>(
      views, workspace, _textCatalog, [this](uimodel::TrackFilterViewState const& state) { applyState(state); });
  }

  void TrackQuickFilterControl::unbind() noexcept
  {
    _commitPending = false;
    _selectionChangedRevoker.revoke();
    _editor = nullptr;
    _viewModelPtr.reset();
    _completerPtr.reset();
    forgetSuggestions();

    if (_debounceTimer)
    {
      _debounceTimer.Stop();
    }
  }

  void TrackQuickFilterControl::resetPresentation()
  {
    if (_input)
    {
      [[maybe_unused]] auto const applyingState = ScopedBooleanFlag{_applyingState};
      _input.IsEnabled(false);
      _input.Text(L"");
      clearSuggestions();
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

    refreshSuggestions();
    schedulePendingText();
  }

  void TrackQuickFilterControl::handleSuggestionChosen(
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxSuggestionChosenEventArgs const& args)
  {
    _optPendingSuggestionIndex = suggestionIndex(args.SelectedItem());
  }

  void TrackQuickFilterControl::handleQuerySubmitted(
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxQuerySubmittedEventArgs const& args)
  {
    if (auto const optIndex = suggestionIndex(args.ChosenSuggestion()); optIndex)
    {
      auto const continuesEditing = quickFilterSuggestionContinuesEditing(_suggestionRows[*optIndex]);

      if (acceptSuggestion(*optIndex) && continuesEditing)
      {
        _debounceTimer.Stop();
        _commitPending = false;

        if (_editor)
        {
          _editor.Focus(winrt::Microsoft::UI::Xaml::FocusState::Keyboard);
        }

        return;
      }
    }

    commitCurrentText();
  }

  void TrackQuickFilterControl::handlePreviewKeyDown(winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
  {
    if (args.Key() == winrt::Windows::System::VirtualKey::Escape && _input.IsSuggestionListOpen())
    {
      clearSuggestions();
      args.Handled(true);
      return;
    }

    if (args.Key() != winrt::Windows::System::VirtualKey::Tab || !_optPendingSuggestionIndex)
    {
      return;
    }

    auto const suggestionIndex = *_optPendingSuggestionIndex;

    if (suggestionIndex >= _suggestionRows.size())
    {
      return;
    }

    auto const continuesEditing = quickFilterSuggestionContinuesEditing(_suggestionRows[suggestionIndex]);

    if (!acceptSuggestion(suggestionIndex))
    {
      return;
    }

    args.Handled(true);

    if (continuesEditing)
    {
      _debounceTimer.Stop();
      _commitPending = false;
    }
    else
    {
      schedulePendingText();
    }

    if (_editor)
    {
      _editor.Focus(winrt::Microsoft::UI::Xaml::FocusState::Keyboard);
    }
  }

  void TrackQuickFilterControl::handleLoaded()
  {
    std::ignore = bindEditor();
  }

  bool TrackQuickFilterControl::bindEditor()
  {
    auto const editor = findTextBox(_input);

    if (!editor)
    {
      return false;
    }

    if (_editor == editor)
    {
      return true;
    }

    _selectionChangedRevoker.revoke();
    _editor = editor;
    _selectionChangedRevoker = _editor.SelectionChanged(
      winrt::auto_revoke,
      [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
      {
        if (!_applyingState)
        {
          refreshSuggestions();
        }
      });
    return true;
  }

  void TrackQuickFilterControl::refreshSuggestions()
  {
    if (_applyingState || !_completerPtr || (!bindEditor() && !_editor))
    {
      clearSuggestions();
      return;
    }

    auto const draft = winrt::to_string(_input.Text());
    auto const optCursor = quickFilterUtf8Cursor(draft, static_cast<std::size_t>(_editor.SelectionStart()));

    if (!optCursor)
    {
      clearSuggestions();
      return;
    }

    auto optResult = _completerPtr->complete(draft, *optCursor);

    if (!optResult || optResult->items.empty())
    {
      clearSuggestions();
      return;
    }

    _completionDraft = draft;
    _suggestionRows = quickFilterSuggestionRows(*optResult, _textCatalog);
    _optCompletionResult = std::move(optResult);
    _optPendingSuggestionIndex.reset();
    _suggestionItems.Clear();

    for (std::size_t index = 0; index < _suggestionRows.size(); ++index)
    {
      _suggestionItems.Append(makeSuggestionElement(_suggestionRows[index], static_cast<std::uint32_t>(index)));
    }

    _input.IsSuggestionListOpen(true);
  }

  void TrackQuickFilterControl::clearSuggestions()
  {
    _input.IsSuggestionListOpen(false);
    _suggestionItems.Clear();
    forgetSuggestions();
  }

  void TrackQuickFilterControl::forgetSuggestions() noexcept
  {
    _suggestionRows.clear();
    _optCompletionResult.reset();
    _completionDraft.clear();
    _optPendingSuggestionIndex.reset();
  }

  std::optional<std::size_t> TrackQuickFilterControl::suggestionIndex(
    winrt::Windows::Foundation::IInspectable const& suggestion) const
  {
    auto const element = suggestion.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>();

    if (!element)
    {
      return std::nullopt;
    }

    auto const property = element.Tag().try_as<winrt::Windows::Foundation::IPropertyValue>();

    if (!property || property.Type() != winrt::Windows::Foundation::PropertyType::UInt32)
    {
      return std::nullopt;
    }

    auto const index = static_cast<std::size_t>(property.GetUInt32());
    return index < _suggestionRows.size() ? std::optional{index} : std::nullopt;
  }

  bool TrackQuickFilterControl::acceptSuggestion(std::size_t const index)
  {
    if (!_optCompletionResult || index >= _suggestionRows.size())
    {
      return false;
    }

    auto const draft = winrt::to_string(_input.Text());

    if (draft != _completionDraft)
    {
      clearSuggestions();
      return false;
    }

    auto const optRange =
      quickFilterUtf16Range(draft, _optCompletionResult->replaceBegin, _optCompletionResult->replaceEnd);

    if (!optRange)
    {
      clearSuggestions();
      return false;
    }

    auto const current = _input.Text();
    auto nativeText = std::wstring{current.c_str(), current.size()};
    auto const replacement = winrt::to_hstring(_suggestionRows[index].insertText);
    nativeText.replace(optRange->begin, optRange->end - optRange->begin, replacement.c_str(), replacement.size());

    auto const caret = optRange->begin + replacement.size();

    if (caret > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    {
      clearSuggestions();
      return false;
    }

    {
      [[maybe_unused]] auto const applyingState = ScopedBooleanFlag{_applyingState};
      _input.Text(winrt::hstring{nativeText});
      clearSuggestions();

      if (bindEditor())
      {
        _editor.Select(static_cast<std::int32_t>(caret), 0);
      }
    }

    return true;
  }

  void TrackQuickFilterControl::schedulePendingText()
  {
    _debounceTimer.Stop();
    _debounceTimer.Interval(kFilterDebounceInterval);
    _commitDeadline = std::chrono::steady_clock::now() + kFilterDebounceInterval;
    _commitPending = true;
    _debounceTimer.Start();
  }

  void TrackQuickFilterControl::commitCurrentText()
  {
    _debounceTimer.Stop();
    _commitDeadline = std::chrono::steady_clock::now();
    _commitPending = true;
    commitPendingText();
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
        clearSuggestions();
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

    if (_onState)
    {
      _onState(state);
    }
  }
} // namespace ao::winui

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "track/TrackPropertiesCoordinator.h"

#include "pch.h"
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/library/detail/TrackCustomMetadata.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/utility/String.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/track/TrackPropertiesAdapter.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::winui
{
  namespace
  {
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using winrt::Windows::Foundation::IInspectable;

    constexpr double kDialogMinWidth = 680.0;
    constexpr double kDialogMaxContentHeight = 640.0;
    constexpr double kSectionSpacing = 12.0;
    constexpr double kRowSpacing = 8.0;
    constexpr double kSectionHeadingFontSize = 18.0;
    constexpr double kSectionHeadingTopMargin = 8.0;
    constexpr double kSectionHeadingBottomMargin = 2.0;
    constexpr double kSupportingTextOpacity = 0.82;
    constexpr double kHintTextOpacity = 0.68;
    constexpr std::size_t kSuggestionLimit = 12;

    TextBlock makeSectionHeading(std::string_view const text)
    {
      auto heading = TextBlock{};
      heading.Text(winrt::to_hstring(text));
      heading.FontSize(kSectionHeadingFontSize);
      heading.Margin(Thickness{
        .Left = 0.0,
        .Top = kSectionHeadingTopMargin,
        .Right = 0.0,
        .Bottom = kSectionHeadingBottomMargin,
      });
      return heading;
    }

    void setSuggestionItems(AutoSuggestBox const& input, std::span<std::string const> const suggestions)
    {
      auto items = winrt::single_threaded_observable_vector<IInspectable>();

      for (auto const& suggestion : suggestions)
      {
        items.Append(winrt::box_value(winrt::to_hstring(suggestion)));
      }

      input.ItemsSource(items);
      input.IsSuggestionListOpen(!suggestions.empty());
    }

    bool containsText(std::span<std::string const> const values, std::string_view const candidate)
    {
      return std::ranges::any_of(values, [candidate](std::string const& value) { return value == candidate; });
    }
  } // namespace

  TrackPropertiesCoordinator::TrackPropertiesCoordinator(TrackPropertiesCoordinatorConfig config)
    : _xamlRoot{std::move(config.xamlRoot)}
    , _asyncRuntime{config.asyncRuntime}
    , _library{config.library}
    , _workspace{config.workspace}
    , _completion{config.completion}
    , _textCatalog{std::move(config.textCatalog)}
    , _trackIds{std::move(config.trackIds)}
    , _formModel{_textCatalog}
    , _formSpec{uimodel::buildTrackPropertiesFormSpec(_textCatalog)}
  {
  }

  TrackPropertiesCoordinator::~TrackPropertiesCoordinator()
  {
    retire();
  }

  Result<> TrackPropertiesCoordinator::present()
  {
    if (_active)
    {
      return {};
    }

    if (!_xamlRoot || !canPresentTrackProperties(_trackIds))
    {
      return makeError(Error::Code::InvalidState, "Track Properties needs an active window and a track selection");
    }

    auto const sessionRes = prepareSession();
    buildDialog();

    if (!sessionRes)
    {
      _sessionInvalid = true;
      setError(i18n::requiredFormat(_textCatalog,
                                    i18n::MessageId::WinUiTrackPropertiesEditingUnavailable,
                                    {{"detail", sessionRes.error().message}}));
    }

    _ownerCallbackGate.renew();
    _active = true;
    updateSaveEnabled();

    try
    {
      _showOperation = _dialog.ShowAsync();
    }
    catch (...)
    {
      _active = false;
      _ownerCallbackGate.retire();
      _sessionInvalidatedSub.reset();
      _optSession.reset();
      throw;
    }

    return {};
  }

  Result<> TrackPropertiesCoordinator::prepareSession()
  {
    _sessionInvalidatedSub.reset();
    _optSession.reset();
    _formModel = uimodel::TrackPropertiesFormModel{_textCatalog};
    _snapshot = {};
    _originalTags.clear();
    _currentTags.clear();
    _sessionInvalid = false;
    _interactionState = InteractionState::Editing;

    auto sessionRes = uimodel::TrackAuthoringSession::begin(_library, _trackIds);

    if (!sessionRes)
    {
      return std::unexpected{sessionRes.error()};
    }

    auto session = std::move(*sessionRes);
    auto baseline = uimodel::TrackPropertiesFormModel{_textCatalog};
    auto tags = std::vector<std::string>{};

    {
      auto snapshot = _library.snapshot();

      if (snapshot.revision() != session.boundRevision())
      {
        return makeError(Error::Code::InvalidState, "The library changed while Track Properties was opening");
      }

      if (auto res = uimodel::loadTrackPropertiesFormBaseline(snapshot, _trackIds, _formSpec, baseline); !res)
      {
        return std::unexpected{res.error()};
      }
      tags = snapshot.selectionTags(_trackIds);
    }

    auto projectionPtr = _workspace.detailProjection(rt::ExplicitSelectionTarget{_trackIds});
    auto detailSnapshot = projectionPtr->snapshot();

    if (detailSnapshot.libraryRevision != session.boundRevision())
    {
      return makeError(Error::Code::InvalidState, "Track Properties detail data is not from the bound selection");
    }

    auto invalidatedSub = session.onInvalidated([this] { handleSessionInvalidated(); });

    if (!session.isCurrent())
    {
      return makeError(Error::Code::InvalidState, "The library changed while Track Properties was opening");
    }

    _optSession.reset();
    _optSession.emplace(std::move(session));
    _sessionInvalidatedSub = std::move(invalidatedSub);
    _formModel = std::move(baseline);
    _snapshot = std::move(detailSnapshot);
    _originalTags = std::move(tags);
    _currentTags = _originalTags;
    return {};
  }

  void TrackPropertiesCoordinator::buildDialog()
  {
    _building = true;
    _fieldEditors.clear();
    _customEditors.clear();

    _dialog = ContentDialog{};
    _dialog.XamlRoot(_xamlRoot);
    _dialog.MinWidth(kDialogMinWidth);
    _dialog.PrimaryButtonText(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesSave)));
    _dialog.CloseButtonText(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesCancel)));
    _dialog.DefaultButton(ContentDialogButton::Primary);

    auto const title =
      _trackIds.size() > 1
        ? i18n::requiredFormat(
            _textCatalog, i18n::MessageId::WinUiTrackPropertiesSelectedTitle, {{"count", _trackIds.size()}})
        : std::string{i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesTitle)};
    _dialog.Title(winrt::box_value(winrt::to_hstring(title)));

    auto content = StackPanel{};
    content.Spacing(kSectionSpacing);

    _errorText = TextBlock{};
    _errorText.TextWrapping(TextWrapping::Wrap);
    _errorText.Visibility(Visibility::Collapsed);
    _errorText.Opacity(kSupportingTextOpacity);
    content.Children().Append(_errorText);

    buildMetadataSection(content);
    buildTagsSection(content);
    buildCustomMetadataSection(content);
    buildTechnicalSection(content);

    auto scroll = ScrollViewer{};
    scroll.MaxHeight(kDialogMaxContentHeight);
    scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scroll.VerticalScrollMode(ScrollMode::Enabled);
    scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    scroll.Content(content);
    _dialog.Content(scroll);

    _primaryClickRevoker = _dialog.PrimaryButtonClick(
      winrt::auto_revoke,
      [this](ContentDialog const&, ContentDialogButtonClickEventArgs const& args) { handleSaveClicked(args); });
    _closedRevoker = _dialog.Closed(
      winrt::auto_revoke, [this](ContentDialog const&, ContentDialogClosedEventArgs const&) { handleClosed(); });
    _building = false;
  }

  void TrackPropertiesCoordinator::buildMetadataSection(StackPanel const& content)
  {
    content.Children().Append(
      makeSectionHeading(i18n::requiredText(_textCatalog, i18n::MessageId::TrackMetadataHeading)));
    for (auto const& row : _formSpec.metadataRows)
    {
      appendFieldEditor(content, row);
    }
  }

  void TrackPropertiesCoordinator::buildTechnicalSection(StackPanel const& content)
  {
    content.Children().Append(
      makeSectionHeading(i18n::requiredText(_textCatalog, i18n::MessageId::TrackAudioPropertiesHeading)));
    for (auto const& row : _formSpec.propertyRows)
    {
      appendFieldEditor(content, row);
    }
  }

  void TrackPropertiesCoordinator::appendFieldEditor(StackPanel const& content,
                                                     uimodel::TrackPropertiesFormRow const& row)
  {
    auto const projection = projectTrackPropertyRow(row, _formModel.rowView(row.field));

    if (projection.controlKind == TrackPropertyControlKind::ReadonlyText)
    {
      auto value = TextBox{};
      value.Header(winrt::box_value(winrt::to_hstring(projection.label)));
      value.Text(winrt::to_hstring(projection.text));
      value.IsReadOnly(true);
      content.Children().Append(value);
      return;
    }

    auto editor = FieldEditor{
      .field = projection.field,
      .controlKind = projection.controlKind,
      .enabled = projection.enabled && _optSession && !_sessionInvalid,
    };

    if (projection.controlKind == TrackPropertyControlKind::Text &&
        rt::supportsTrackFieldValueCompletion(projection.field))
    {
      editor.suggestBox = AutoSuggestBox{};
      editor.suggestBox.Header(winrt::box_value(winrt::to_hstring(projection.label)));
      editor.suggestBox.UpdateTextOnSelect(true);
      editor.suggestBox.IsEnabled(editor.enabled);

      if (projection.mixed)
      {
        editor.suggestBox.PlaceholderText(winrt::to_hstring(projection.text));
      }
      else
      {
        editor.suggestBox.Text(winrt::to_hstring(projection.text));
      }

      auto const field = editor.field;
      editor.suggestionChangedRevoker = editor.suggestBox.TextChanged(
        winrt::auto_revoke,
        [this, field](AutoSuggestBox const&, AutoSuggestBoxTextChangedEventArgs const& args)
        {
          if (_building)
          {
            return;
          }

          auto const iter = std::ranges::find(_fieldEditors, field, &FieldEditor::field);

          if (iter != _fieldEditors.end() && args.Reason() == AutoSuggestionBoxTextChangeReason::UserInput)
          {
            refreshMetadataSuggestions(*iter);
          }

          updateSaveEnabled();
        });
      content.Children().Append(editor.suggestBox);
    }
    else
    {
      editor.textBox = TextBox{};
      editor.textBox.Header(winrt::box_value(winrt::to_hstring(projection.label)));
      editor.textBox.IsEnabled(editor.enabled);

      if (projection.mixed)
      {
        editor.textBox.PlaceholderText(winrt::to_hstring(projection.text));
      }
      else
      {
        editor.textBox.Text(winrt::to_hstring(projection.text));
      }

      editor.textChangedRevoker = editor.textBox.TextChanged(winrt::auto_revoke,
                                                             [this](IInspectable const&, TextChangedEventArgs const&)
                                                             {
                                                               if (!_building)
                                                               {
                                                                 updateSaveEnabled();
                                                               }
                                                             });
      content.Children().Append(editor.textBox);
    }

    _fieldEditors.push_back(std::move(editor));
  }

  void TrackPropertiesCoordinator::refreshMetadataSuggestions(FieldEditor const& editor)
  {
    if (!editor.suggestBox)
    {
      return;
    }

    auto completer = rt::MetadataValueCompleter{_completion, editor.field};
    auto const completionItems = completer.complete(winrt::to_string(editor.suggestBox.Text()), kSuggestionLimit);
    auto suggestions = std::vector<std::string>{};
    suggestions.reserve(completionItems.size());

    for (auto const& item : completionItems)
    {
      suggestions.push_back(item.insertText);
    }

    setSuggestionItems(editor.suggestBox, suggestions);
  }

  std::string TrackPropertiesCoordinator::editorText(FieldEditor const& editor) const
  {
    if (editor.suggestBox)
    {
      return winrt::to_string(editor.suggestBox.Text());
    }

    return editor.textBox ? winrt::to_string(editor.textBox.Text()) : std::string{};
  }

  void TrackPropertiesCoordinator::buildTagsSection(StackPanel const& content)
  {
    content.Children().Append(
      makeSectionHeading(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesTags)));

    auto hint = TextBlock{};
    hint.Text(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesTagsHint)));
    hint.TextWrapping(TextWrapping::Wrap);
    hint.Opacity(kHintTextOpacity);
    content.Children().Append(hint);

    _tagRows = StackPanel{};
    _tagRows.Spacing(kRowSpacing);
    content.Children().Append(_tagRows);
    rebuildTagRows();

    auto addRow = Grid{};
    auto inputColumn = ColumnDefinition{};
    inputColumn.Width(GridLength{.Value = 1.0, .GridUnitType = GridUnitType::Star});
    auto buttonColumn = ColumnDefinition{};
    buttonColumn.Width(GridLength{.Value = 0.0, .GridUnitType = GridUnitType::Auto});
    addRow.ColumnDefinitions().Append(inputColumn);
    addRow.ColumnDefinitions().Append(buttonColumn);
    addRow.ColumnSpacing(kRowSpacing);

    _tagInput = AutoSuggestBox{};
    _tagInput.PlaceholderText(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesTags)));
    _tagInput.IsEnabled(_optSession && !_sessionInvalid);
    _tagTextChangedRevoker =
      _tagInput.TextChanged(winrt::auto_revoke,
                            [this](AutoSuggestBox const&, AutoSuggestBoxTextChangedEventArgs const& args)
                            {
                              if (!_building && args.Reason() == AutoSuggestionBoxTextChangeReason::UserInput)
                              {
                                refreshTagSuggestions();
                              }
                            });
    _tagSubmittedRevoker = _tagInput.QuerySubmitted(
      winrt::auto_revoke, [this](AutoSuggestBox const&, AutoSuggestBoxQuerySubmittedEventArgs const&) { addTag(); });
    addRow.Children().Append(_tagInput);

    _tagAddButton = Button{};
    _tagAddButton.Content(
      winrt::box_value(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesAdd))));
    _tagAddButton.IsEnabled(_optSession && !_sessionInvalid);
    _tagAddClickRevoker =
      _tagAddButton.Click(winrt::auto_revoke, [this](IInspectable const&, RoutedEventArgs const&) { addTag(); });
    Grid::SetColumn(_tagAddButton, 1);
    addRow.Children().Append(_tagAddButton);
    content.Children().Append(addRow);
  }

  void TrackPropertiesCoordinator::rebuildTagRows()
  {
    if (!_tagRows)
    {
      return;
    }

    _tagRemoveButtons.clear();
    _tagRemoveClickRevokers.clear();
    _tagRows.Children().Clear();

    for (auto const& tag : _currentTags)
    {
      auto row = Grid{};
      auto textColumn = ColumnDefinition{};
      textColumn.Width(GridLength{.Value = 1.0, .GridUnitType = GridUnitType::Star});
      auto buttonColumn = ColumnDefinition{};
      buttonColumn.Width(GridLength{.Value = 0.0, .GridUnitType = GridUnitType::Auto});
      row.ColumnDefinitions().Append(textColumn);
      row.ColumnDefinitions().Append(buttonColumn);

      auto label = TextBlock{};
      label.Text(winrt::to_hstring(tag));
      label.VerticalAlignment(VerticalAlignment::Center);
      row.Children().Append(label);

      auto remove = Button{};
      remove.Content(winrt::box_value(
        winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesDelete))));
      remove.IsEnabled(_optSession && !_sessionInvalid && _interactionState == InteractionState::Editing);
      _tagRemoveClickRevokers.push_back(remove.Click(winrt::auto_revoke,
                                                     [this, tag](IInspectable const&, RoutedEventArgs const&)
                                                     {
                                                       if (_interactionState != InteractionState::Editing ||
                                                           _sessionInvalid)
                                                       {
                                                         return;
                                                       }

                                                       std::erase(_currentTags, tag);
                                                       rebuildTagRows();
                                                       updateSaveEnabled();
                                                     }));
      Grid::SetColumn(remove, 1);
      row.Children().Append(remove);
      _tagRemoveButtons.push_back(remove);
      _tagRows.Children().Append(row);
    }
  }

  void TrackPropertiesCoordinator::refreshTagSuggestions()
  {
    if (!_tagInput)
    {
      return;
    }

    auto suggestions =
      trackPropertyVocabularySuggestions(_completion.tags(), winrt::to_string(_tagInput.Text()), kSuggestionLimit);
    std::erase_if(suggestions, [this](std::string const& value) { return containsText(_currentTags, value); });
    setSuggestionItems(_tagInput, suggestions);
  }

  void TrackPropertiesCoordinator::addTag()
  {
    if (!_tagInput || _sessionInvalid || _interactionState != InteractionState::Editing)
    {
      return;
    }

    auto const inputText = winrt::to_string(_tagInput.Text());
    auto const trimmed = utility::trim(inputText);

    if (trimmed.empty())
    {
      return;
    }

    if (!containsText(_currentTags, trimmed))
    {
      _currentTags.emplace_back(trimmed);
      rebuildTagRows();
    }

    _tagInput.Text(L"");
    setSuggestionItems(_tagInput, std::span<std::string const>{});
    updateSaveEnabled();
  }

  void TrackPropertiesCoordinator::buildCustomMetadataSection(StackPanel const& content)
  {
    content.Children().Append(
      makeSectionHeading(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesCustomMetadata)));
    _customRows = StackPanel{};
    _customRows.Spacing(kRowSpacing);
    content.Children().Append(_customRows);

    for (auto const& item : _snapshot.customMetadata)
    {
      appendCustomEditor(item);
    }

    auto addRow = Grid{};
    auto keyColumn = ColumnDefinition{};
    keyColumn.Width(GridLength{.Value = 1.0, .GridUnitType = GridUnitType::Star});
    auto valueColumn = ColumnDefinition{};
    valueColumn.Width(GridLength{.Value = 1.0, .GridUnitType = GridUnitType::Star});
    auto buttonColumn = ColumnDefinition{};
    buttonColumn.Width(GridLength{.Value = 0.0, .GridUnitType = GridUnitType::Auto});
    addRow.ColumnDefinitions().Append(keyColumn);
    addRow.ColumnDefinitions().Append(valueColumn);
    addRow.ColumnDefinitions().Append(buttonColumn);
    addRow.ColumnSpacing(kRowSpacing);

    _customKeyInput = AutoSuggestBox{};
    _customKeyInput.PlaceholderText(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesCustomKey)));
    _customKeyInput.IsEnabled(_optSession && !_sessionInvalid);
    _customKeyChangedRevoker =
      _customKeyInput.TextChanged(winrt::auto_revoke,
                                  [this](AutoSuggestBox const&, AutoSuggestBoxTextChangedEventArgs const& args)
                                  {
                                    if (!_building && args.Reason() == AutoSuggestionBoxTextChangeReason::UserInput)
                                    {
                                      refreshCustomKeySuggestions();
                                    }
                                  });
    addRow.Children().Append(_customKeyInput);

    _customValueInput = TextBox{};
    _customValueInput.PlaceholderText(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesCustomValue)));
    _customValueInput.IsEnabled(_optSession && !_sessionInvalid);
    Grid::SetColumn(_customValueInput, 1);
    addRow.Children().Append(_customValueInput);

    _customAddButton = Button{};
    _customAddButton.Content(
      winrt::box_value(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesAdd))));
    _customAddButton.IsEnabled(_optSession && !_sessionInvalid);
    _customAddClickRevoker = _customAddButton.Click(
      winrt::auto_revoke, [this](IInspectable const&, RoutedEventArgs const&) { addCustomMetadata(); });
    Grid::SetColumn(_customAddButton, 2);
    addRow.Children().Append(_customAddButton);
    content.Children().Append(addRow);
  }

  void TrackPropertiesCoordinator::appendCustomEditor(rt::CustomMetadataItem const& item)
  {
    auto panel = Grid{};
    auto valueColumn = ColumnDefinition{};
    valueColumn.Width(GridLength{.Value = 1.0, .GridUnitType = GridUnitType::Star});
    auto buttonColumn = ColumnDefinition{};
    buttonColumn.Width(GridLength{.Value = 0.0, .GridUnitType = GridUnitType::Auto});
    panel.ColumnDefinitions().Append(valueColumn);
    panel.ColumnDefinitions().Append(buttonColumn);
    panel.ColumnSpacing(kRowSpacing);

    auto value = TextBox{};
    value.Header(winrt::box_value(winrt::to_hstring(item.key)));
    auto const editable = item.presentOnAll && !item.value.mixed;
    auto const display = editable ? item.value.optValue.value_or("")
                                  : std::string{i18n::requiredText(_textCatalog, i18n::MessageId::TrackMultipleValues)};
    value.Text(winrt::to_hstring(display));
    value.IsEnabled(editable && _optSession && !_sessionInvalid);
    panel.Children().Append(value);

    auto remove = Button{};
    remove.Content(winrt::box_value(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesDelete))));
    remove.VerticalAlignment(VerticalAlignment::Bottom);
    remove.IsEnabled(_optSession && !_sessionInvalid);
    auto const index = _customEditors.size();
    auto deleteClickRevoker = remove.Click(
      winrt::auto_revoke, [this, index](IInspectable const&, RoutedEventArgs const&) { deleteCustomMetadata(index); });
    Grid::SetColumn(remove, 1);
    panel.Children().Append(remove);
    _customRows.Children().Append(panel);

    auto valueChangedRevoker = value.TextChanged(winrt::auto_revoke,
                                                 [this](IInspectable const&, TextChangedEventArgs const&)
                                                 {
                                                   if (!_building)
                                                   {
                                                     updateSaveEnabled();
                                                   }
                                                 });
    _customEditors.push_back(CustomEditor{
      .key = item.key,
      .optOriginalValue = editable ? item.value.optValue : std::nullopt,
      .panel = panel,
      .value = value,
      .remove = remove,
      .valueChangedRevoker = std::move(valueChangedRevoker),
      .deleteClickRevoker = std::move(deleteClickRevoker),
      .existed = true,
      .editable = editable,
    });
  }

  void TrackPropertiesCoordinator::appendNewCustomEditor(std::string key, std::string valueText)
  {
    auto item = rt::CustomMetadataItem{
      .key = key,
      .value = {.optValue = valueText},
      .presentOnAll = true,
      .presentOnAny = true,
    };
    appendCustomEditor(item);
    auto& editor = _customEditors.back();
    editor.existed = false;
    editor.optOriginalValue.reset();
  }

  void TrackPropertiesCoordinator::refreshCustomKeySuggestions()
  {
    if (!_customKeyInput)
    {
      return;
    }

    auto suggestions = trackPropertyVocabularySuggestions(
      _completion.customKeys(), winrt::to_string(_customKeyInput.Text()), kSuggestionLimit);
    std::erase_if(suggestions,
                  [this](std::string const& value)
                  {
                    return std::ranges::any_of(_customEditors,
                                               [&value](CustomEditor const& editor)
                                               { return !editor.deleted && editor.key == value; });
                  });
    setSuggestionItems(_customKeyInput, suggestions);
  }

  void TrackPropertiesCoordinator::addCustomMetadata()
  {
    if (!_customKeyInput || !_customValueInput || _sessionInvalid || _interactionState != InteractionState::Editing)
    {
      return;
    }

    auto const keyText = winrt::to_string(_customKeyInput.Text());
    auto const keyView = utility::trim(keyText);
    auto const existing =
      std::ranges::find_if(_customEditors, [keyView](CustomEditor const& editor) { return editor.key == keyView; });

    if (keyView.empty() || (existing != _customEditors.end() && !existing->deleted))
    {
      setError(i18n::requiredFormat(
        _textCatalog, i18n::MessageId::WinUiTrackPropertiesInvalidValue, {{"detail", std::string{keyView}}}));
      return;
    }

    if (existing != _customEditors.end())
    {
      existing->deleted = false;
      existing->editable = true;
      existing->panel.Visibility(Visibility::Visible);
      updateEditorEnabled();
      existing->value.Text(_customValueInput.Text());
      _customKeyInput.Text(L"");
      _customValueInput.Text(L"");
      setSuggestionItems(_customKeyInput, std::span<std::string const>{});
      clearError();
      updateSaveEnabled();
      return;
    }

    auto const validation = uimodel::validateCustomMetadataAddition(_snapshot, keyView);

    if (validation != uimodel::CustomMetadataAddValidation::Accepted)
    {
      setError(i18n::requiredFormat(
        _textCatalog, i18n::MessageId::WinUiTrackPropertiesInvalidValue, {{"detail", std::string{keyView}}}));
      return;
    }

    appendNewCustomEditor(std::string{keyView}, winrt::to_string(_customValueInput.Text()));
    _customKeyInput.Text(L"");
    _customValueInput.Text(L"");
    setSuggestionItems(_customKeyInput, std::span<std::string const>{});
    updateSaveEnabled();
  }

  void TrackPropertiesCoordinator::deleteCustomMetadata(std::size_t const index)
  {
    if (index >= _customEditors.size() || _sessionInvalid || _interactionState != InteractionState::Editing)
    {
      return;
    }

    auto& editor = _customEditors[index];
    editor.deleted = true;
    editor.panel.Visibility(Visibility::Collapsed);
    updateEditorEnabled();
    updateSaveEnabled();
  }

  bool TrackPropertiesCoordinator::trySynchronizeFieldEdits()
  {
    for (auto const& editor : _fieldEditors)
    {
      if (!editor.enabled)
      {
        continue;
      }

      auto editRes = parseTrackPropertyEdit(editor.controlKind, editorText(editor));

      if (!editRes)
      {
        setError(i18n::requiredFormat(
          _textCatalog, i18n::MessageId::WinUiTrackPropertiesInvalidValue, {{"detail", editRes.error().message}}));
        return false;
      }

      _formModel.setEditValue(editor.field, std::move(*editRes));
    }

    clearError();
    return true;
  }

  bool TrackPropertiesCoordinator::hasPendingChanges() const
  {
    if (_formModel.canSave())
    {
      return true;
    }

    for (auto const& editor : _customEditors)
    {
      if ((editor.deleted && editor.existed) ||
          (!editor.deleted && editor.editable &&
           needsCustomMetadataValueUpdate(
             editor.existed, editor.optOriginalValue, winrt::to_string(editor.value.Text()))))
      {
        return true;
      }
    }

    return !tagsToAdd().empty() || !tagsToRemove().empty();
  }

  rt::MetadataPatch TrackPropertiesCoordinator::buildMetadataPatch() const
  {
    auto patch = _formModel.buildPatch();

    for (auto const& editor : _customEditors)
    {
      if (editor.deleted)
      {
        if (editor.existed)
        {
          patch.customUpdates[editor.key] = std::nullopt;
        }

        continue;
      }

      if (!editor.editable)
      {
        continue;
      }

      if (auto value = winrt::to_string(editor.value.Text());
          needsCustomMetadataValueUpdate(editor.existed, editor.optOriginalValue, value))
      {
        patch.customUpdates[editor.key] = std::move(value);
      }
    }

    return patch;
  }

  std::vector<std::string> TrackPropertiesCoordinator::tagsToAdd() const
  {
    auto result = std::vector<std::string>{};

    for (auto const& tag : _currentTags)
    {
      if (!containsText(_originalTags, tag))
      {
        result.push_back(tag);
      }
    }

    return result;
  }

  std::vector<std::string> TrackPropertiesCoordinator::tagsToRemove() const
  {
    auto result = std::vector<std::string>{};

    for (auto const& tag : _originalTags)
    {
      if (!containsText(_currentTags, tag))
      {
        result.push_back(tag);
      }
    }

    return result;
  }

  void TrackPropertiesCoordinator::setInteractionState(InteractionState const state)
  {
    _interactionState = state;

    if (state != InteractionState::Editing)
    {
      for (auto const& editor : _fieldEditors)
      {
        if (editor.suggestBox)
        {
          editor.suggestBox.IsSuggestionListOpen(false);
        }
      }

      if (_tagInput)
      {
        _tagInput.IsSuggestionListOpen(false);
      }

      if (_customKeyInput)
      {
        _customKeyInput.IsSuggestionListOpen(false);
      }
    }

    updateEditorEnabled();
    updateSaveEnabled();
  }

  void TrackPropertiesCoordinator::updateEditorEnabled()
  {
    auto const canEdit = _interactionState == InteractionState::Editing && _optSession && !_sessionInvalid;

    for (auto const& editor : _fieldEditors)
    {
      if (editor.suggestBox)
      {
        editor.suggestBox.IsEnabled(canEdit && editor.enabled);
      }
      else if (editor.textBox)
      {
        editor.textBox.IsEnabled(canEdit && editor.enabled);
      }
    }

    if (_tagInput)
    {
      _tagInput.IsEnabled(canEdit);
    }

    if (_tagAddButton)
    {
      _tagAddButton.IsEnabled(canEdit);
    }

    for (auto const& remove : _tagRemoveButtons)
    {
      remove.IsEnabled(canEdit);
    }

    if (_customKeyInput)
    {
      _customKeyInput.IsEnabled(canEdit);
    }

    if (_customValueInput)
    {
      _customValueInput.IsEnabled(canEdit);
    }

    if (_customAddButton)
    {
      _customAddButton.IsEnabled(canEdit);
    }

    for (auto const& editor : _customEditors)
    {
      editor.value.IsEnabled(canEdit && editor.editable && !editor.deleted);
      editor.remove.IsEnabled(canEdit && !editor.deleted);
    }
  }

  void TrackPropertiesCoordinator::updateSaveEnabled()
  {
    if (_building || !_dialog)
    {
      return;
    }

    if (_interactionState != InteractionState::Editing)
    {
      _dialog.IsPrimaryButtonEnabled(false);
      return;
    }

    auto const valid = !_sessionInvalid && _optSession && _optSession->isCurrent() && trySynchronizeFieldEdits();
    _dialog.IsPrimaryButtonEnabled(valid && hasPendingChanges());
  }

  void TrackPropertiesCoordinator::setError(std::string text)
  {
    if (!_errorText)
    {
      return;
    }

    _errorText.Text(winrt::to_hstring(text));
    _errorText.Visibility(Visibility::Visible);
  }

  void TrackPropertiesCoordinator::clearError()
  {
    if (!_errorText || _sessionInvalid)
    {
      return;
    }

    _errorText.Text(L"");
    _errorText.Visibility(Visibility::Collapsed);
  }

  void TrackPropertiesCoordinator::handleSessionInvalidated()
  {
    if (!_active)
    {
      return;
    }

    _sessionInvalid = true;
    setError(std::string{i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesStale)});
    updateEditorEnabled();
    updateSaveEnabled();
  }

  void TrackPropertiesCoordinator::handleSaveClicked(ContentDialogButtonClickEventArgs const& args)
  {
    // ContentDialog primary clicks close synchronously by default. The shared
    // authoring transaction decides whether this draft may close, so keep the
    // dialog open until the callback-executor completion says it was accepted.
    args.Cancel(true);

    if (_interactionState != InteractionState::Editing || _sessionInvalid || !_optSession ||
        !trySynchronizeFieldEdits() || !hasPendingChanges())
    {
      updateSaveEnabled();
      return;
    }

    auto metadataPatch = buildMetadataPatch();
    auto addTags = tagsToAdd();
    auto removeTags = tagsToRemove();

    clearError();
    setInteractionState(InteractionState::Submitting);

    auto submission = submitChangesAsync(_optSession->submitPropertiesAsync(rt::TrackPropertiesPatch{
      .metadata = std::move(metadataPatch),
      .tagsToAdd = std::move(addTags),
      .tagsToRemove = std::move(removeTags),
    }));
    auto const token = _ownerCallbackGate.token();
    _asyncRuntime.spawnWithLifetime(
      _tasks,
      [runtime = &_asyncRuntime, owner = this, token, submission = std::move(submission)](
        std::stop_token const stopToken) mutable
      { return runSaveWorkflowAsync(runtime, owner, token, std::move(submission), stopToken); },
      "Windows track-properties save");
  }

  async::Task<Result<TrackPropertiesCommitState>> TrackPropertiesCoordinator::submitChangesAsync(
    async::Task<Result<uimodel::TrackPropertiesSubmitResult>> submission)
  {
    auto propertiesRes = co_await std::move(submission);

    if (!propertiesRes)
    {
      co_return std::unexpected{propertiesRes.error()};
    }

    co_return projectTrackPropertiesCommitState(propertiesRes->status);
  }

  async::Task<void> TrackPropertiesCoordinator::runSaveWorkflowAsync(
    async::Runtime* const runtime,
    TrackPropertiesCoordinator* const owner,
    CallbackAdmissionGate::Token token,
    async::Task<Result<TrackPropertiesCommitState>> submission,
    std::stop_token const stopToken)
  {
    auto res = co_await std::move(submission);
    co_await runtime->resumeOnCallbackExecutorAsync(stopToken);

    if (token.accepts())
    {
      owner->finishSave(std::move(res));
    }
  }

  void TrackPropertiesCoordinator::finishSave(Result<TrackPropertiesCommitState> res)
  {
    if (!_active)
    {
      return;
    }

    if (!res)
    {
      setInteractionState(InteractionState::Editing);
      setError(i18n::requiredFormat(
        _textCatalog, i18n::MessageId::WinUiTrackPropertiesSaveFailed, {{"detail", res.error().message}}));
      return;
    }

    switch (*res)
    {
      case TrackPropertiesCommitState::Accepted:
        setInteractionState(InteractionState::Closing);

        if (_dialog)
        {
          _dialog.Hide();
        }

        return;
      case TrackPropertiesCommitState::Busy:
        setInteractionState(InteractionState::Editing);
        setError(std::string{i18n::requiredText(_textCatalog, i18n::MessageId::LibraryBusyTryAgain)});
        return;
      case TrackPropertiesCommitState::Stale:
      case TrackPropertiesCommitState::Unavailable:
        _sessionInvalid = true;
        setInteractionState(InteractionState::Editing);
        setError(std::string{i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesStale)});
        return;
    }
  }

  void TrackPropertiesCoordinator::handleClosed()
  {
    _active = false;
    _interactionState = InteractionState::Closing;
    _ownerCallbackGate.retire();
    _tasks.cancelAll();
    _sessionInvalidatedSub.reset();
    _optSession.reset();
    _primaryClickRevoker.revoke();
    _closedRevoker.revoke();
    _tagTextChangedRevoker.revoke();
    _tagSubmittedRevoker.revoke();
    _tagAddClickRevoker.revoke();
    _tagRemoveButtons.clear();
    _tagRemoveClickRevokers.clear();
    _customKeyChangedRevoker.revoke();
    _customAddClickRevoker.revoke();
    _showOperation = nullptr;
    _dialog = nullptr;
    _errorText = nullptr;
    _tagRows = nullptr;
    _tagInput = nullptr;
    _tagAddButton = nullptr;
    _customRows = nullptr;
    _customKeyInput = nullptr;
    _customValueInput = nullptr;
    _customAddButton = nullptr;
    _fieldEditors.clear();
    _customEditors.clear();
  }

  void TrackPropertiesCoordinator::retire() noexcept
  {
    _active = false;
    _interactionState = InteractionState::Closing;
    _ownerCallbackGate.retire();
    _tasks.cancelAll();
    _sessionInvalidatedSub.reset();
    _optSession.reset();

    _primaryClickRevoker.revoke();
    _closedRevoker.revoke();
    _tagTextChangedRevoker.revoke();
    _tagSubmittedRevoker.revoke();
    _tagAddClickRevoker.revoke();
    _tagRemoveButtons.clear();
    _tagRemoveClickRevokers.clear();
    _customKeyChangedRevoker.revoke();
    _customAddClickRevoker.revoke();

    if (_dialog)
    {
      // Callback admission is already closed, so native dismissal is
      // presentation-only cleanup rather than an operation invariant.
      runOptionalWinRt("hiding the WinUI track-properties dialog", [this] { _dialog.Hide(); });
    }

    _showOperation = nullptr;
    _dialog = nullptr;
    _errorText = nullptr;
    _tagRows = nullptr;
    _tagInput = nullptr;
    _tagAddButton = nullptr;
    _customRows = nullptr;
    _customKeyInput = nullptr;
    _customValueInput = nullptr;
    _customAddButton = nullptr;
    _fieldEditors.clear();
    _customEditors.clear();
  }
} // namespace ao::winui

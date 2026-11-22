// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "track/TrackPropertiesCoordinator.h"

#include "track/TrackCreditsEditorControl.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/detail/TrackCustomMetadata.h>
#include <ao/uimodel/library/property/TagEdit.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/property/TrackPropertiesPreparation.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>
#include <ao/winui/CallbackAdmissionGate.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/track/TrackPropertiesAdapter.h>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

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
#include <vector>

namespace ao::winui
{
  namespace
  {
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using winrt::Windows::Foundation::IInspectable;

    constexpr double kDialogMinWidth = 320.0;
    constexpr double kDialogMaxContentHeight = 640.0;
    constexpr double kDialogMaxFeedbackHeight = 120.0;
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
    , _optCreditsScope{config.optCreditsScope}
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
      _optFrozenError = i18n::requiredFormat(_textCatalog,
                                             i18n::MessageId::WinUiTrackPropertiesEditingUnavailable,
                                             {{"detail", sessionRes.error().message}});
    }

    _ownerCallbackGate.renew();
    _active = true;

    if (_optCreditsScope && sessionRes)
    {
      beginCreditsEdit(*_optCreditsScope);
    }

    updateEditorEnabled();
    updateDialogFeedback();

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

    auto baseline = uimodel::TrackPropertiesFormModel{_textCatalog};
    auto tags = std::vector<std::string>{};
    auto sessionRes = [&] -> Result<uimodel::TrackAuthoringSession>
    {
      auto snapshot = _library.snapshot();
      auto boundRes = uimodel::prepareTrackProperties(_library, snapshot, _trackIds, _formSpec, baseline);

      if (!boundRes)
      {
        return std::unexpected{boundRes.error()};
      }

      tags = snapshot.selectionTags(boundRes->targetIds());
      return std::move(*boundRes);
    }();

    if (!sessionRes)
    {
      return std::unexpected{sessionRes.error()};
    }

    auto session = std::move(*sessionRes);

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
    _dialog = ContentDialog{};
    _dialog.XamlRoot(_xamlRoot);
    _dialog.MinWidth(kDialogMinWidth);
    _dialog.PrimaryButtonText(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesSave)));
    _dialog.CloseButtonText(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesCancel)));
    _dialog.DefaultButton(ContentDialogButton::Primary);

    auto const targetCount = _optSession ? _formModel.targetCount() : _trackIds.size();
    auto const title = targetCount > 1
                         ? i18n::requiredFormat(
                             _textCatalog, i18n::MessageId::WinUiTrackPropertiesSelectedTitle, {{"count", targetCount}})
                         : std::string{i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesTitle)};
    _dialog.Title(winrt::box_value(winrt::to_hstring(title)));

    buildDialogContent();

    _primaryClickRevoker = _dialog.PrimaryButtonClick(
      winrt::auto_revoke,
      [this](ContentDialog const&, ContentDialogButtonClickEventArgs const& args) { handleSaveClicked(args); });
    _closingRevoker = _dialog.Closing(winrt::auto_revoke,
                                      [this](ContentDialog const&, ContentDialogClosingEventArgs const& args)
                                      {
                                        if (_interactionState == InteractionState::Closing)
                                        {
                                          return;
                                        }

                                        if (_interactionState == InteractionState::Submitting)
                                        {
                                          args.Cancel(true);
                                          return;
                                        }

                                        if (_discardPrompt.Visibility() == Visibility::Visible)
                                        {
                                          args.Cancel(true);
                                          cancelDiscard();
                                          return;
                                        }

                                        if (needsDiscardDecision())
                                        {
                                          args.Cancel(true);
                                          requestDiscard(false);
                                        }
                                      });
    _closedRevoker = _dialog.Closed(
      winrt::auto_revoke, [this](ContentDialog const&, ContentDialogClosedEventArgs const&) { handleClosed(); });
  }

  void TrackPropertiesCoordinator::buildDialogContent()
  {
    _building = true;
    _fieldEditors.clear();
    _customEditors.clear();
    _optFrozenError.reset();
    _optAdditionRejection.reset();
    _retryFeedback = TrackPropertiesRetryFeedback::None;

    auto content = StackPanel{};
    content.Spacing(kSectionSpacing);

    buildCreditsSection(content);
    buildMetadataSection(content);
    buildTagsSection(content);
    buildCustomMetadataSection(content);
    buildTechnicalSection(content);

    auto scroll = ScrollViewer{};
    scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scroll.VerticalScrollMode(ScrollMode::Enabled);
    scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    scroll.Content(content);

    auto layout = Grid{};
    layout.MaxHeight(kDialogMaxContentHeight);
    auto fieldsRow = RowDefinition{};
    fieldsRow.Height(GridLength{.Value = 1.0, .GridUnitType = GridUnitType::Star});
    auto feedbackRow = RowDefinition{};
    feedbackRow.Height(GridLength{.Value = 0.0, .GridUnitType = GridUnitType::Auto});
    layout.RowDefinitions().Append(fieldsRow);
    layout.RowDefinitions().Append(feedbackRow);
    auto recoveryRow = RowDefinition{};
    recoveryRow.Height(GridLength{.Value = 0.0, .GridUnitType = GridUnitType::Auto});
    layout.RowDefinitions().Append(recoveryRow);
    layout.Children().Append(scroll);

    // Keep the reason Save is blocked visible when the user is far down the form.
    auto feedback = StackPanel{};
    _errorText = TextBlock{};
    _errorText.TextWrapping(TextWrapping::Wrap);
    _errorText.Visibility(Visibility::Collapsed);
    _errorText.Opacity(kSupportingTextOpacity);
    feedback.Children().Append(_errorText);
    _unstagedInputHint = TextBlock{};
    _unstagedInputHint.Text(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::TrackPropertiesUnstagedInputHint)));
    _unstagedInputHint.TextWrapping(TextWrapping::Wrap);
    _unstagedInputHint.Margin(Thickness{.Left = 0.0, .Top = kRowSpacing, .Right = 0.0, .Bottom = 0.0});
    _unstagedInputHint.Visibility(Visibility::Collapsed);
    feedback.Children().Append(_unstagedInputHint);

    // Long diagnostics can include user input. Keep the fields reachable without truncating the error.
    auto feedbackScroll = ScrollViewer{};
    feedbackScroll.MaxHeight(kDialogMaxFeedbackHeight);
    feedbackScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    feedbackScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    feedbackScroll.Content(feedback);
    Grid::SetRow(feedbackScroll, 1);
    layout.Children().Append(feedbackScroll);
    // Keep Reload reachable even when the form and its diagnostic are scrolled independently.
    Grid::SetRow(_reloadButton, 2);
    layout.Children().Append(_reloadButton);
    _dialog.Content(layout);
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

    for (auto const& row : _formModel.segmentRows())
    {
      auto value = TextBox{};
      value.Header(winrt::box_value(winrt::to_hstring(row.label)));
      value.Text(winrt::to_hstring(row.text));
      value.IsReadOnly(true);
      value.TextWrapping(TextWrapping::Wrap);
      content.Children().Append(value);
    }
  }

  void TrackPropertiesCoordinator::appendFieldEditor(StackPanel const& content,
                                                     uimodel::TrackPropertiesFormRow const& row)
  {
    // Complete category lists and scoped edit actions live in the Credits section.
    if (rt::creditKindForTrackField(row.field))
    {
      return;
    }

    auto const projection = projectTrackPropertyRow(row, _formModel.rowView(row.field));

    if (projection.controlKind == uimodel::TrackPropertiesFormEditorKind::ReadonlyText)
    {
      auto value = TextBox{};
      value.Header(winrt::box_value(winrt::to_hstring(projection.label)));
      value.Text(winrt::to_hstring(projection.text));
      value.IsReadOnly(true);
      content.Children().Append(value);
      return;
    }

    auto editor = FieldEditor{.row = projection};
    auto const targetCount = _formModel.targetCount();
    auto const placeholder =
      targetCount > 1 && projection.placeholder.empty() ? projection.text : projection.placeholder;

    if (projection.controlKind == uimodel::TrackPropertiesFormEditorKind::Text &&
        rt::supportsTrackFieldValueCompletion(projection.field))
    {
      editor.suggestBox = AutoSuggestBox{};
      editor.suggestBox.Header(winrt::box_value(winrt::to_hstring(projection.label)));
      editor.suggestBox.UpdateTextOnSelect(true);
      editor.suggestBox.PlaceholderText(winrt::to_hstring(placeholder));
      editor.suggestBox.Text(winrt::to_hstring(projection.text));

      auto const field = row.field;
      editor.suggestionChangedRevoker = editor.suggestBox.TextChanged(
        winrt::auto_revoke,
        [this, field](AutoSuggestBox const& sender, AutoSuggestBoxTextChangedEventArgs const& args)
        {
          if (_building)
          {
            return;
          }

          auto const iter =
            std::ranges::find_if(_fieldEditors, [field](FieldEditor const& item) { return item.row.field == field; });

          if (iter != _fieldEditors.end() && iter->suggestBox == sender && sender.IsEnabled() &&
              !iter->isClearRequested() && args.Reason() == AutoSuggestionBoxTextChangeReason::UserInput &&
              args.CheckCurrent())
          {
            refreshMetadataSuggestions(*iter);
          }

          updateDialogFeedback();
        });
      content.Children().Append(editor.suggestBox);
    }
    else
    {
      editor.textBox = TextBox{};
      editor.textBox.Header(winrt::box_value(winrt::to_hstring(projection.label)));
      editor.textBox.PlaceholderText(winrt::to_hstring(placeholder));
      editor.textBox.Text(winrt::to_hstring(projection.text));

      editor.textChangedRevoker = editor.textBox.TextChanged(winrt::auto_revoke,
                                                             [this](IInspectable const&, TextChangedEventArgs const&)
                                                             {
                                                               if (!_building)
                                                               {
                                                                 updateDialogFeedback();
                                                               }
                                                             });

      if (targetCount > 1 && projection.enabled &&
          (projection.controlKind == uimodel::TrackPropertiesFormEditorKind::Number ||
           projection.controlKind == uimodel::TrackPropertiesFormEditorKind::Date))
      {
        editor.lostFocusRevoker = editor.textBox.LostFocus(winrt::auto_revoke,
                                                           [this](IInspectable const& sender, RoutedEventArgs const&)
                                                           { restoreWithdrawnInput(sender.as<TextBox>()); });
      }

      content.Children().Append(editor.textBox);
    }

    if (projection.enabled && (targetCount > 1 || (targetCount == 1 && row.field == rt::TrackField::Title)))
    {
      auto label = TextBlock{};
      label.Text(winrt::to_hstring(i18n::requiredFormat(_textCatalog,
                                                        i18n::MessageId::TrackClearFieldForTracks,
                                                        {{"field", projection.label}, {"count", targetCount}})));
      label.TextWrapping(TextWrapping::Wrap);
      editor.clearCheck = CheckBox{};
      editor.clearCheck.Content(label);
      Automation::AutomationProperties::SetName(editor.clearCheck, label.Text());
      auto toggled = [this](IInspectable const& sender, RoutedEventArgs const&)
      { handleClearToggled(sender.as<CheckBox>()); };
      editor.checkedRevoker = editor.clearCheck.Checked(winrt::auto_revoke, toggled);
      editor.uncheckedRevoker = editor.clearCheck.Unchecked(winrt::auto_revoke, toggled);
      content.Children().Append(editor.clearCheck);
    }

    _fieldEditors.push_back(std::move(editor));
  }

  bool TrackPropertiesCoordinator::acceptsFieldGesture() const
  {
    return !_building && _active && _interactionState == InteractionState::Editing && !_sessionInvalid && _optSession &&
           _optSession->isCurrent() && (!_discardPrompt || _discardPrompt.Visibility() != Visibility::Visible);
  }

  void TrackPropertiesCoordinator::restoreWithdrawnInput(TextBox const& source)
  {
    if (!acceptsFieldGesture() || !source.IsEnabled() || source.FocusState() != FocusState::Unfocused)
    {
      return;
    }

    auto const iter = std::ranges::find(_fieldEditors, source, &FieldEditor::textBox);

    if (iter == _fieldEditors.end() || iter->isClearRequested())
    {
      return;
    }

    // A typed zero or whitespace-only date can withdraw a batch edit even though it
    // hides the placeholder. Wait until focus leaves so ordinary typing is uninterrupted.
    if (auto const text = winrt::to_string(source.Text()); text != iter->row.text)
    {
      auto const valueRes = uimodel::parseTrackPropertiesFieldInput(iter->row.controlKind, text);

      if (valueRes && uimodel::isEmptyTrackFieldEdit(*valueRes))
      {
        source.Text(winrt::to_hstring(iter->row.text));
      }
    }
  }

  void TrackPropertiesCoordinator::handleClearToggled(CheckBox const& source)
  {
    if (!acceptsFieldGesture() || !source.IsEnabled())
    {
      return;
    }

    auto const iter = std::ranges::find(_fieldEditors, source, &FieldEditor::clearCheck);

    if (iter == _fieldEditors.end())
    {
      return;
    }

    // Never replay a previous replacement (or invalid text). TextChanged sees only the final
    // value, with the checkbox already checked for Clear or unchecked for the captured baseline.
    auto const text = source.IsChecked().GetBoolean() ? winrt::hstring{} : winrt::to_hstring(iter->row.text);
    auto const token = _ownerCallbackGate.token();

    if (auto const input = iter->suggestBox; input)
    {
      input.Text(text);
      input.IsSuggestionListOpen(false);
    }
    else
    {
      auto const textBox = iter->textBox;
      textBox.Text(text);
    }

    if (token.accepts())
    {
      updateEditorEnabled();
    }

    if (token.accepts())
    {
      updateDialogFeedback();
    }
  }

  void TrackPropertiesCoordinator::refreshMetadataSuggestions(FieldEditor const& editor)
  {
    if (!editor.suggestBox)
    {
      return;
    }

    auto completer = rt::MetadataValueCompleter{_completion, editor.row.field};
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
                              if (acceptsFieldGesture() && args.CheckCurrent())
                              {
                                clearAdditionRejection(Addition::Tag);

                                if (args.Reason() == AutoSuggestionBoxTextChangeReason::UserInput)
                                {
                                  refreshTagSuggestions();
                                }
                              }

                              updateDialogFeedback();
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
                                                       updateDialogFeedback();
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
    if (!_tagInput || !acceptsFieldGesture())
    {
      return;
    }

    auto const input = winrt::to_string(_tagInput.Text());

    if (auto const trimmed = utility::trim(input); !trimmed.empty())
    {
      auto const addedRes = uimodel::appendCommonTag(_currentTags, trimmed);

      if (!addedRes)
      {
        updateDialogFeedback(
          i18n::requiredText(_textCatalog, i18n::MessageId::TrackPropertiesTextNormalizationFailed), Addition::Tag);
        return;
      }

      if (*addedRes)
      {
        rebuildTagRows();
      }
    }

    _tagInput.Text(L"");
    setSuggestionItems(_tagInput, std::span<std::string const>{});
    updateDialogFeedback();
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
                                    if (acceptsFieldGesture() && args.CheckCurrent())
                                    {
                                      clearAdditionRejection(Addition::CustomMetadata);

                                      if (args.Reason() == AutoSuggestionBoxTextChangeReason::UserInput)
                                      {
                                        refreshCustomKeySuggestions();
                                      }
                                    }

                                    updateDialogFeedback();
                                  });
    addRow.Children().Append(_customKeyInput);

    _customValueInput = TextBox{};
    _customValueInput.PlaceholderText(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesCustomValue)));
    _customValueInput.IsEnabled(_optSession && !_sessionInvalid);
    _customValueChangedRevoker = _customValueInput.TextChanged(
      winrt::auto_revoke, [this](IInspectable const&, TextChangedEventArgs const&) { updateDialogFeedback(); });
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
                                                     updateDialogFeedback();
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
    if (!_customKeyInput || !_customValueInput || !acceptsFieldGesture())
    {
      return;
    }

    auto const keyText = winrt::to_string(_customKeyInput.Text());
    auto keyRes = utility::normalizeUtf8Nfc(utility::trim(keyText));

    if (!keyRes)
    {
      updateDialogFeedback(i18n::requiredText(_textCatalog, i18n::MessageId::TrackPropertiesTextNormalizationFailed),
                           Addition::CustomMetadata);
      return;
    }

    auto const existing = std::ranges::find(_customEditors, *keyRes, &CustomEditor::key);
    auto const addingNew = existing == _customEditors.end();
    auto const knownKey = !addingNew && existing->deleted;
    auto const newKey = addingNew && uimodel::validateCustomMetadataAddition(_snapshot, *keyRes) ==
                                       uimodel::CustomMetadataAddValidation::Accepted;

    if (keyRes->empty() || (!knownKey && !newKey))
    {
      updateDialogFeedback(
        i18n::requiredFormat(_textCatalog, i18n::MessageId::WinUiTrackPropertiesInvalidValue, {{"detail", *keyRes}}),
        Addition::CustomMetadata);
      return;
    }

    if (!addingNew)
    {
      existing->deleted = false;
      existing->editable = true;
      existing->panel.Visibility(Visibility::Visible);
      updateEditorEnabled();
      existing->value.Text(_customValueInput.Text());
    }
    else
    {
      appendNewCustomEditor(std::move(*keyRes), winrt::to_string(_customValueInput.Text()));
    }

    _customKeyInput.Text(L"");
    _customValueInput.Text(L"");
    setSuggestionItems(_customKeyInput, std::span<std::string const>{});
    updateDialogFeedback();
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
    updateDialogFeedback();
  }

  std::optional<uimodel::TrackPropertiesFormEditorKind> TrackPropertiesCoordinator::synchronizeFieldEdits(
    uimodel::TrackPropertiesFormModel& form) const
  {
    auto optInvalidKind = std::optional<uimodel::TrackPropertiesFormEditorKind>{};

    // Keep all typed edits current even when an earlier raw input is invalid.
    for (auto const& editor : _fieldEditors)
    {
      if (!editor.row.enabled)
      {
        continue;
      }

      auto const editRes =
        synchronizeTrackPropertyInput(form, editor.row, editorText(editor), editor.isClearRequested());

      if (!editRes && !optInvalidKind)
      {
        optInvalidKind = editor.row.controlKind;
      }
    }

    return optInvalidKind;
  }

  bool TrackPropertiesCoordinator::hasAuxiliaryChanges() const
  {
    if (!uimodel::diffCommonTags(_originalTags, _currentTags).empty())
    {
      return true;
    }

    return std::ranges::any_of(
      _customEditors,
      [](CustomEditor const& editor)
      {
        return editor.deleted
                 ? editor.existed
                 : editor.editable && needsCustomMetadataValueUpdate(
                                        editor.existed, editor.optOriginalValue, winrt::to_string(editor.value.Text()));
      });
  }

  bool TrackPropertiesCoordinator::hasUnstagedTagInput() const
  {
    return _tagInput && !_tagInput.Text().empty();
  }

  bool TrackPropertiesCoordinator::hasUnstagedCustomInput() const
  {
    return (_customKeyInput && !_customKeyInput.Text().empty()) ||
           (_customValueInput && !_customValueInput.Text().empty());
  }

  bool TrackPropertiesCoordinator::hasUnstagedInput() const
  {
    return hasUnstagedTagInput() || hasUnstagedCustomInput();
  }

  bool TrackPropertiesCoordinator::needsDiscardDecision() const
  {
    if (hasUnstagedInput() || hasAuxiliaryChanges() || _formModel.creditsEditor().hasDraftChanges())
    {
      return true;
    }

    // Check late raw input without changing the live draft, even after the session is stale.
    auto candidate = _formModel;
    return synchronizeFieldEdits(candidate).has_value() || candidate.hasEffectiveChanges();
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
          uimodel::setCustomMetadataPatchEntry(patch, editor.key, std::nullopt);
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
        uimodel::setCustomMetadataPatchEntry(patch, editor.key, std::move(value));
      }
    }

    return patch;
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
    updateDialogFeedback();
  }

  void TrackPropertiesCoordinator::FieldEditor::setEnabled(bool const enabled) const
  {
    if (clearCheck)
    {
      clearCheck.IsEnabled(enabled);
    }

    if (auto const inputEnabled = enabled && !isClearRequested(); suggestBox)
    {
      suggestBox.IsEnabled(inputEnabled);
    }
    else if (textBox)
    {
      textBox.IsEnabled(inputEnabled);
    }
  }

  void TrackPropertiesCoordinator::updateEditorEnabled()
  {
    auto const canInteract = _interactionState == InteractionState::Editing &&
                             (!_discardPrompt || _discardPrompt.Visibility() != Visibility::Visible);
    auto const canEdit = canInteract && _optSession && !_sessionInvalid;

    if (_creditsControlPtr)
    {
      _creditsControlPtr->setEnabled(canEdit);
    }

    if (_creditsPreview)
    {
      for (auto const& child : _creditsPreview.Children())
      {
        if (auto control = child.try_as<Control>(); control)
        {
          control.IsEnabled(canEdit && !_formModel.creditsEditor().isEditing());
        }
      }
    }

    if (_reloadButton)
    {
      _reloadButton.Visibility(_sessionInvalid ? Visibility::Visible : Visibility::Collapsed);
      _reloadButton.IsEnabled(canInteract);
    }

    for (auto const& editor : _fieldEditors)
    {
      editor.setEnabled(canEdit && editor.row.enabled);
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

  TrackPropertiesCoordinator::DraftFeedback TrackPropertiesCoordinator::synchronizeDraftFeedback(
    std::string_view const actionError,
    std::optional<Addition> const optRejectedAddition)
  {
    if (!acceptsFieldGesture())
    {
      return {};
    }

    auto const pendingTag = hasUnstagedTagInput();
    auto const pendingCustom = hasUnstagedCustomInput();

    if (optRejectedAddition)
    {
      _optAdditionRejection = AdditionRejection{.kind = *optRejectedAddition, .message = std::string{actionError}};
    }
    else if (_optAdditionRejection && !(_optAdditionRejection->kind == Addition::Tag ? pendingTag : pendingCustom))
    {
      _optAdditionRejection.reset();
    }

    auto const optRejected = _optAdditionRejection ? std::optional{_optAdditionRejection->kind} : std::nullopt;
    auto draft = DraftFeedback{
      .optInvalidKind = synchronizeFieldEdits(_formModel),
      .showPendingHint =
        (pendingTag && optRejected != Addition::Tag) || (pendingCustom && optRejected != Addition::CustomMetadata),
    };

    // Invalid raw input can withdraw the only staged change without making the draft clean.
    if (draft.optInvalidKind)
    {
      return draft;
    }

    if (auto const auxiliaryDirty = hasAuxiliaryChanges();
        !pendingTag && !pendingCustom && canSubmitTrackProperties(_formModel, auxiliaryDirty))
    {
      draft.retryDraft = TrackPropertiesRetryDraft::SubmitReady;
    }
    else if (!auxiliaryDirty && !_formModel.hasEffectiveChanges())
    {
      draft.retryDraft = TrackPropertiesRetryDraft::Unchanged;
    }

    return draft;
  }

  winrt::hstring TrackPropertiesCoordinator::dialogFeedbackText(DraftFeedback const& draft,
                                                                bool const bindingCurrent,
                                                                std::string_view const actionError) const
  {
    if (_interactionState == InteractionState::Closing)
    {
      return {};
    }

    if (_optFrozenError)
    {
      return winrt::to_hstring(*_optFrozenError);
    }

    if (_sessionInvalid || (_optSession && !bindingCurrent))
    {
      return winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackPropertiesStale));
    }

    if (!acceptsFieldGesture())
    {
      return {};
    }

    if (_optAdditionRejection)
    {
      return winrt::to_hstring(_optAdditionRejection->message);
    }

    if (draft.optInvalidKind)
    {
      auto const hint = uimodel::trackPropertiesInputHint(_textCatalog, *draft.optInvalidKind);
      return winrt::to_hstring(
        i18n::requiredFormat(_textCatalog, i18n::MessageId::WinUiTrackPropertiesInvalidValue, {{"detail", hint}}));
    }

    if (!actionError.empty())
    {
      return winrt::to_hstring(actionError);
    }

    if (_retryFeedback == TrackPropertiesRetryFeedback::Visible)
    {
      return winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::LibraryBusyTryAgain));
    }

    return {};
  }

  TrackPropertiesRetryDraft TrackPropertiesCoordinator::updateDialogFeedback(
    std::string_view const actionError,
    std::optional<Addition> const optRejectedAddition)
  {
    if (_building || !_dialog || !_errorText || !_unstagedInputHint)
    {
      return TrackPropertiesRetryDraft::Blocked;
    }

    auto const draft = synchronizeDraftFeedback(actionError, optRejectedAddition);
    auto const bindingCurrent = !_sessionInvalid && _optSession && _optSession->isCurrent();
    _retryFeedback = projectTrackPropertiesRetryFeedback(_retryFeedback, bindingCurrent, draft.retryDraft);

    auto const feedback = dialogFeedbackText(draft, bindingCurrent, actionError);
    _errorText.Text(feedback);
    _errorText.Visibility(feedback.empty() ? Visibility::Collapsed : Visibility::Visible);
    _unstagedInputHint.Visibility(draft.showPendingHint ? Visibility::Visible : Visibility::Collapsed);
    _dialog.IsPrimaryButtonEnabled(draft.retryDraft == TrackPropertiesRetryDraft::SubmitReady);
    return draft.retryDraft;
  }

  void TrackPropertiesCoordinator::clearAdditionRejection(Addition const kind)
  {
    if (_optAdditionRejection && _optAdditionRejection->kind == kind)
    {
      _optAdditionRejection.reset();
    }
  }

  void TrackPropertiesCoordinator::handleSessionInvalidated()
  {
    if (!_active)
    {
      return;
    }

    _sessionInvalid = true;
    updateEditorEnabled();
    updateDialogFeedback();
  }

  void TrackPropertiesCoordinator::handleSaveClicked(ContentDialogButtonClickEventArgs const& args)
  {
    // ContentDialog primary clicks close synchronously by default. The shared
    // authoring transaction decides whether this draft may close, so keep the
    // dialog open until the callback-executor completion says it was accepted.
    args.Cancel(true);

    if (updateDialogFeedback() != TrackPropertiesRetryDraft::SubmitReady)
    {
      return;
    }

    auto metadataPatch = buildMetadataPatch();
    auto tags = uimodel::diffCommonTags(_originalTags, _currentTags);

    _optAdditionRejection.reset();
    _retryFeedback = TrackPropertiesRetryFeedback::None;
    setInteractionState(InteractionState::Submitting);
    AO_INVARIANT(_optSession, "A submitting Properties dialog must retain its authoring session");
    auto& session = *_optSession;

    auto submission = session.submitPropertiesAsync(rt::TrackPropertiesPatch{
      .metadata = std::move(metadataPatch),
      .tagsToAdd = std::move(tags.tagsToAdd),
      .tagsToRemove = std::move(tags.tagsToRemove),
    });
    auto const token = _ownerCallbackGate.token();
    _asyncRuntime.spawnWithLifetime(
      _tasks,
      [runtime = &_asyncRuntime, owner = this, token, submission = std::move(submission)](
        std::stop_token const stopToken) mutable
      { return runSaveWorkflowAsync(runtime, owner, token, std::move(submission), stopToken); },
      "Windows track-properties save");
  }

  async::Task<void> TrackPropertiesCoordinator::runSaveWorkflowAsync(
    async::Runtime* const runtime,
    TrackPropertiesCoordinator* const owner,
    CallbackAdmissionGate::Token token,
    async::Task<Result<uimodel::TrackPropertiesSubmitResult>> submission,
    std::stop_token const stopToken)
  {
    auto res = co_await std::move(submission);
    co_await runtime->resumeOnCallbackExecutorAsync(stopToken);

    if (token.accepts())
    {
      owner->finishSave(std::move(res));
    }
  }

  void TrackPropertiesCoordinator::finishSave(Result<uimodel::TrackPropertiesSubmitResult> res)
  {
    if (!_active)
    {
      return;
    }

    if (!res)
    {
      _sessionInvalid = true;
      _optFrozenError = i18n::requiredFormat(
        _textCatalog, i18n::MessageId::WinUiTrackPropertiesSaveFailed, {{"detail", res.error().message}});
      setInteractionState(InteractionState::Editing);
      return;
    }

    auto const bindingCurrent = !_sessionInvalid && _optSession && _optSession->isCurrent();

    switch (projectTrackPropertiesCommitState(res->status, bindingCurrent))
    {
      case TrackPropertiesCommitState::Accepted:
        setInteractionState(InteractionState::Closing);

        if (_dialog)
        {
          _dialog.Hide();
        }

        return;
      case TrackPropertiesCommitState::Busy:
        _retryFeedback = TrackPropertiesRetryFeedback::Deferred;
        setInteractionState(InteractionState::Editing);
        return;
      case TrackPropertiesCommitState::Stale:
      case TrackPropertiesCommitState::Unavailable:
        _sessionInvalid = true;
        setInteractionState(InteractionState::Editing);
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
    _creditsControlPtr.reset();
    _creditsHost = nullptr;
    _creditsPreview = nullptr;
    _discardPrompt = nullptr;
    _reloadButton = nullptr;
    _formModel.cancelCreditsEdit();
    _creditClickRevokers.clear();
    _discardClickRevokers.clear();
    _closingRevoker.revoke();
    _primaryClickRevoker.revoke();
    _closedRevoker.revoke();
    _tagTextChangedRevoker.revoke();
    _tagSubmittedRevoker.revoke();
    _tagAddClickRevoker.revoke();
    _tagRemoveButtons.clear();
    _tagRemoveClickRevokers.clear();
    _customKeyChangedRevoker.revoke();
    _customAddClickRevoker.revoke();
    _customValueChangedRevoker.revoke();
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
    _unstagedInputHint = nullptr;
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
    _creditsControlPtr.reset();
    _creditsHost = nullptr;
    _creditsPreview = nullptr;
    _discardPrompt = nullptr;
    _reloadButton = nullptr;
    _formModel.cancelCreditsEdit();
    _creditClickRevokers.clear();
    _discardClickRevokers.clear();
    _closingRevoker.revoke();

    _primaryClickRevoker.revoke();
    _closedRevoker.revoke();
    _tagTextChangedRevoker.revoke();
    _tagSubmittedRevoker.revoke();
    _tagAddClickRevoker.revoke();
    _tagRemoveButtons.clear();
    _tagRemoveClickRevokers.clear();
    _customKeyChangedRevoker.revoke();
    _customAddClickRevoker.revoke();
    _customValueChangedRevoker.revoke();

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
    _unstagedInputHint = nullptr;
    _fieldEditors.clear();
    _customEditors.clear();
  }
} // namespace ao::winui

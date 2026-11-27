// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "list/ListAuthoringCoordinator.h"

#include "track/TrackListController.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/library/list/ListAuthoring.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/list/ListOrderOutcome.h>
#include <ao/uimodel/library/list/ListOrderSession.h>
#include <ao/uimodel/library/list/SmartListEditing.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

// Asio can introduce the Win32 macro before the XAML projections are parsed.
#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#pragma pop_macro("GetCurrentTime")
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
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

    constexpr auto kPreviewDebounceInterval = std::chrono::milliseconds{100};
    constexpr double kEditorMinWidth = 620.0;
    constexpr double kDialogMaxHeight = 680.0;
    constexpr double kSectionSpacing = 12.0;
    constexpr double kSupportingTextOpacity = 0.82;
    constexpr double kContextTextOpacity = 0.72;
    constexpr double kPreviewHeadingFontSize = 18.0;

    TextBlock makeWrappedText(double const opacity = 1.0)
    {
      auto text = TextBlock{};
      text.TextWrapping(TextWrapping::Wrap);
      text.Opacity(opacity);
      return text;
    }

    std::string labeled(std::string_view const label, std::string_view const value)
    {
      return std::format("{}: {}", label, value);
    }

    bool shouldRefreshListPreview(rt::LibraryChangeSet const& change, ListId const parentListId)
    {
      // Ancestor changes can affect the count or diagnostic even if the direct parent's definition is unchanged.
      return change.libraryReset || !change.tracksInserted.empty() || !change.tracksDeleted.empty() ||
             !change.tracksMutated.empty() ||
             (!rt::isVirtualListId(parentListId) && (!change.listsUpserted.empty() || !change.listsDeleted.empty()));
    }
  } // namespace

  ListAuthoringCoordinator::ListAuthoringCoordinator(ListAuthoringCoordinatorConfig config)
    : _xamlRoot{std::move(config.xamlRoot)}
    , _asyncRuntime{config.asyncRuntime}
    , _editing{config.editing}
    , _views{config.views}
    , _sources{config.sources}
    , _notifications{config.notifications}
    , _trackList{config.trackList}
    , _presentationCatalog{config.presentationCatalog}
    , _listPresentations{config.listPresentations}
    , _textOrderingPolicy{config.textOrderingPolicy}
    , _textCatalog{std::move(config.textCatalog)}
    , _reportStatus{std::move(config.reportStatus)}
  {
    // Owner callbacks are admitted for the coordinator's whole lifetime, but
    // dialog callbacks open only after beginDialogWorkflow creates their scope.
    _dialogGenerationGate.retire();
  }

  ListAuthoringCoordinator::~ListAuthoringCoordinator()
  {
    retire();
  }

  void ListAuthoringCoordinator::createList(ListId const parentListId, std::string initialExpression)
  {
    if (_retired || _dialogActive)
    {
      return;
    }

    presentEditor(parentListId, kInvalidListId, {}, {}, std::move(initialExpression));
  }

  void ListAuthoringCoordinator::editList(ListId const listId)
  {
    if (_retired || _dialogActive || rt::isVirtualListId(listId))
    {
      return;
    }

    auto const optNode = _editing.library().snapshot().listNode(listId);

    if (!optNode)
    {
      reportFeedback(rt::NotificationSeverity::Error,
                     std::string{i18n::requiredText(_textCatalog, i18n::MessageId::ListOrderListUnavailable)},
                     rt::NotificationLifetime::history());
      return;
    }

    presentEditor(optNode->parentId, listId, optNode->name, optNode->description, optNode->expression);
  }

  void ListAuthoringCoordinator::presentEditor(ListId const parentListId,
                                               ListId const editListId,
                                               std::string name,
                                               std::string description,
                                               std::string expression)
  {
    beginDialogWorkflow();
    _parentListId = parentListId;
    _editListId = editListId;
    _mutationError.clear();

    buildEditorDialog();
    _nameInput.Text(winrt::to_hstring(name));
    _descriptionInput.Text(winrt::to_hstring(description));
    _filterInput.Text(winrt::to_hstring(expression));

    std::int32_t selectedPresentationIndex = 0;

    if (auto const optId = _listPresentations.presentationIdForList(editListId); optId)
    {
      auto const selected = std::ranges::find(_presentationIds, optId);

      if (selected != _presentationIds.end())
      {
        selectedPresentationIndex = static_cast<std::int32_t>(std::distance(_presentationIds.begin(), selected));
      }
    }

    _presentationInput.SelectedIndex(selectedPresentationIndex);
    tryValidateEditorDraft(editorDraft());
    showDialog();
  }

  void ListAuthoringCoordinator::buildEditorDialog()
  {
    _dialog = ContentDialog{};
    _dialog.XamlRoot(_xamlRoot ? _xamlRoot() : XamlRoot{nullptr});
    _dialog.MinWidth(kEditorMinWidth);
    _dialog.Title(winrt::box_value(winrt::to_hstring(i18n::requiredText(
      _textCatalog,
      _editListId == kInvalidListId ? i18n::MessageId::ListEditorNewTitle : i18n::MessageId::ListEditorEditTitle))));
    _dialog.PrimaryButtonText(winrt::to_hstring(i18n::requiredText(
      _textCatalog, _editListId == kInvalidListId ? i18n::MessageId::CommonCreate : i18n::MessageId::CommonSave)));
    _dialog.CloseButtonText(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::CommonCancel)));
    _dialog.DefaultButton(ContentDialogButton::Primary);

    auto content = StackPanel{};
    content.Spacing(kSectionSpacing);

    _errorText = makeWrappedText(kSupportingTextOpacity);
    _errorText.Visibility(Visibility::Collapsed);
    content.Children().Append(_errorText);

    _nameInput = TextBox{};
    _nameInput.Header(
      winrt::box_value(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::ListFieldName))));
    _nameInput.PlaceholderText(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::ListFieldNamePlaceholder)));
    content.Children().Append(_nameInput);

    _descriptionInput = TextBox{};
    _descriptionInput.Header(
      winrt::box_value(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::ListFieldDescription))));
    _descriptionInput.PlaceholderText(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::ListFieldDescriptionPlaceholder)));
    content.Children().Append(_descriptionInput);

    _inheritedFilterText = makeWrappedText(kContextTextOpacity);
    content.Children().Append(_inheritedFilterText);

    _filterInput = TextBox{};
    _filterInput.Header(
      winrt::box_value(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::ListFieldLocalFilter))));
    _filterInput.PlaceholderText(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::ListFieldFilterPlaceholder)));
    content.Children().Append(_filterInput);

    _effectiveFilterText = makeWrappedText(kContextTextOpacity);
    _membershipText = makeWrappedText(kContextTextOpacity);
    content.Children().Append(_effectiveFilterText);
    content.Children().Append(_membershipText);

    _presentationInput = ComboBox{};
    _presentationInput.Header(winrt::box_value(
      winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::TrackPresentationPickerLabel))));
    _presentationInput.HorizontalAlignment(HorizontalAlignment::Stretch);
    _presentationIds.clear();
    _presentationIds.emplace_back(std::nullopt);
    _presentationInput.Items().Append(
      winrt::box_value(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::TrackPresentationAuto))));

    for (auto const& item : _presentationCatalog.menuItems())
    {
      if (item.type != uimodel::TrackPresentationMenuItemType::Preset)
      {
        continue;
      }

      if (auto const optId = rt::parseTrackPresetId(item.id); optId)
      {
        _presentationIds.push_back(optId);
        _presentationInput.Items().Append(winrt::box_value(winrt::to_hstring(item.label)));
      }
    }

    content.Children().Append(_presentationInput);

    auto previewHeading = TextBlock{};
    previewHeading.Text(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::ListFieldPreview)));
    previewHeading.FontSize(kPreviewHeadingFontSize);
    content.Children().Append(previewHeading);
    _previewText = makeWrappedText(kSupportingTextOpacity);
    Automation::AutomationProperties::SetAutomationId(_previewText, L"list-editor-preview-status");
    content.Children().Append(_previewText);

    auto scroll = ScrollViewer{};
    scroll.MaxHeight(kDialogMaxHeight);
    scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scroll.VerticalScrollMode(ScrollMode::Enabled);
    scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    scroll.Content(content);
    _dialog.Content(scroll);

    _previewTimer = _filterInput.DispatcherQueue().CreateTimer();
    _previewTimer.Interval(kPreviewDebounceInterval);
    _previewTimer.IsRepeating(false);
    auto const token = _dialogGenerationGate.token();
    _previewTickRevoker = _previewTimer.Tick(winrt::auto_revoke,
                                             [this, token](auto const&, IInspectable const&)
                                             {
                                               if (token.accepts() && !_submitting)
                                               {
                                                 tryValidateEditorDraft(editorDraft());
                                               }
                                             });
    _previewChanges = _editing.library().changes().onChanged(
      [this, token](rt::LibraryChangeSet const& change)
      {
        if (!token.accepts() || _submitting || _previewTimer.IsRunning())
        {
          return;
        }

        // The replica has applied the commit.
        if (shouldRefreshListPreview(change, _parentListId))
        {
          _previewTimer.Start();
        }
      });
    _nameChangedRevoker = _nameInput.TextChanged(winrt::auto_revoke,
                                                 [this, token](IInspectable const&, TextChangedEventArgs const&)
                                                 {
                                                   if (token.accepts() && !_submitting)
                                                   {
                                                     _mutationError.clear();
                                                     tryValidateEditorDraft(editorDraft());
                                                   }
                                                 });
    _descriptionChangedRevoker =
      _descriptionInput.TextChanged(winrt::auto_revoke,
                                    [this, token](IInspectable const&, TextChangedEventArgs const&)
                                    {
                                      if (token.accepts() && !_submitting && !_mutationError.empty())
                                      {
                                        _mutationError.clear();
                                        tryValidateEditorDraft(editorDraft());
                                      }
                                    });
    _filterChangedRevoker = _filterInput.TextChanged(winrt::auto_revoke,
                                                     [this, token](IInspectable const&, TextChangedEventArgs const&)
                                                     {
                                                       if (token.accepts() && !_submitting)
                                                       {
                                                         _mutationError.clear();
                                                         _previewTimer.Stop();
                                                         _previewTimer.Start();
                                                       }
                                                     });
    _primaryClickRevoker =
      _dialog.PrimaryButtonClick(winrt::auto_revoke,
                                 [this, token](ContentDialog const&, ContentDialogButtonClickEventArgs const& args)
                                 {
                                   if (token.accepts())
                                   {
                                     handleEditorSave(args);
                                   }
                                 });
    _closedRevoker = _dialog.Closed(winrt::auto_revoke,
                                    [this, token](ContentDialog const&, ContentDialogClosedEventArgs const&)
                                    {
                                      if (token.accepts())
                                      {
                                        handleDialogClosed();
                                      }
                                    });
  }

  rt::ListDraft ListAuthoringCoordinator::editorDraft() const
  {
    return uimodel::makeSmartListDraft(_parentListId,
                                       _editListId,
                                       winrt::to_string(_nameInput.Text()),
                                       winrt::to_string(_descriptionInput.Text()),
                                       winrt::to_string(_filterInput.Text()));
  }

  bool ListAuthoringCoordinator::tryValidateEditorDraft(rt::ListDraft const& draft)
  {
    _previewTimer.Stop();
    auto const& expression = draft.expression;
    auto const sourceListId = rt::resolveParentSourceId(draft.parentId);
    auto previewRes = _sources.acquire(rt::SourceSpec{.baseListId = sourceListId, .filterExpression = expression});
    auto optPreviewError = std::optional<Error>{};
    auto input = uimodel::SmartListPreviewState{
      .name = draft.name,
      .localExpression = expression,
      .hasPreviewSource = previewRes.has_value(),
      .isAllTracks = sourceListId == rt::kAllTracksListId,
    };

    if (previewRes)
    {
      // SmartListPreviewState borrows its strings. Keep the owning Error alive
      // until makeSmartListEditorViewState has copied the diagnostic.
      optPreviewError = _sources.sourceError(*previewRes);
      input.hasError = optPreviewError.has_value();
      input.errorMessage = optPreviewError ? std::string_view{optPreviewError->message} : std::string_view{};
      input.matchCount = previewRes->source().size();
      // The cache holds ad-hoc sources weakly. Keep this expression live so
      // a track commit updates its membership instead of forcing a new scan.
      _optPreviewLease.emplace(std::move(*previewRes));
    }
    else
    {
      _optPreviewLease.reset();
      input.errorMessage = previewRes.error().message;
    }

    auto const isRoot = rt::isVirtualListId(draft.parentId);
    auto const optParent = isRoot ? std::nullopt : _editing.library().snapshot().listNode(draft.parentId);
    auto const parentExpression = optParent ? std::string_view{optParent->expression} : std::string_view{};
    auto const missingParent = !isRoot && !optParent;
    auto const unavailable = i18n::requiredText(_textCatalog, i18n::MessageId::ListOrderListUnavailable);

    if (missingParent && input.hasPreviewSource)
    {
      input.hasPreviewSource = false;
      input.errorMessage = unavailable;
    }

    auto const editorState = uimodel::makeSmartListEditorViewState(_textCatalog, input);
    auto const inherited = missingParent
                             ? std::string{unavailable}
                             : uimodel::formatSmartListExpressionDisplayText(_textCatalog, parentExpression);
    auto const effective =
      missingParent ? std::string{unavailable}
                    : uimodel::formatSmartListExpressionDisplayText(
                        _textCatalog, uimodel::combineSmartListEffectiveExpression(parentExpression, expression));
    _inheritedFilterText.Text(winrt::to_hstring(
      labeled(i18n::requiredText(_textCatalog, i18n::MessageId::ListFieldInheritedFilter), inherited)));
    _effectiveFilterText.Text(winrt::to_hstring(
      labeled(i18n::requiredText(_textCatalog, i18n::MessageId::ListFieldEffectiveFilter), effective)));
    _membershipText.Text(winrt::to_hstring(labeled(
      i18n::requiredText(_textCatalog, i18n::MessageId::ListFieldMembership), editorState.membershipEditingText)));
    _previewText.Text(winrt::to_hstring(editorState.previewStatusText));

    setDialogError(uimodel::formatSmartListEditorErrorText(_mutationError, editorState));
    _dialog.IsPrimaryButtonEnabled(editorState.canSubmit);
    return editorState.canSubmit;
  }

  void ListAuthoringCoordinator::handleEditorSave(ContentDialogButtonClickEventArgs const& args)
  {
    args.Cancel(true);

    if (_submitting || !_optDialogTasks || !_dialogGenerationGate.isOpen())
    {
      return;
    }

    auto const token = _dialogGenerationGate.token();
    _mutationError.clear();

    auto draft = editorDraft();
    auto const selected = _presentationInput.SelectedIndex();

    if (!tryValidateEditorDraft(draft) || !_optDialogTasks || !token.accepts())
    {
      return;
    }

    _submitting = true;
    _dialog.IsPrimaryButtonEnabled(false);

    if (!_optDialogTasks || !token.accepts())
    {
      return;
    }

    auto finish = [name = draft.name, expression = draft.expression, selected](
                    ListAuthoringCoordinator* target, Result<ListId> res)
    { target->finishEditorSave(std::move(res), name, expression, selected); };
    auto submission = uimodel::saveListAsync(_editing, std::move(draft));
    _asyncRuntime.spawnWithLifetime(
      *_optDialogTasks,
      [runtime = &_asyncRuntime, owner = this, token, submission = std::move(submission), finish = std::move(finish)](
        std::stop_token const stopToken) mutable
      {
        return finishOnCallbackExecutorAsync(
          runtime, owner, token, std::move(submission), std::move(finish), stopToken);
      },
      "Windows List save");
  }

  void ListAuthoringCoordinator::finishEditorSave(Result<ListId> res,
                                                  std::string const& name,
                                                  std::string const& expression,
                                                  std::int32_t const selected)
  {
    if (!res)
    {
      _submitting = false;
      _mutationError = res.error().message;
      tryValidateEditorDraft(editorDraft());
      return;
    }

    auto const token = _dialogGenerationGate.token();

    auto const optPresetId = selected >= 0 && static_cast<std::size_t>(selected) < _presentationIds.size()
                               ? _presentationIds[static_cast<std::size_t>(selected)]
                               : std::nullopt;

    if (optPresetId)
    {
      _listPresentations.setPresentationIdForList(*res, *optPresetId);
    }
    else
    {
      _listPresentations.clearPresentationForList(*res);
    }

    if (!token.accepts())
    {
      return;
    }

    auto presentation =
      rt::NavigationPresentation{.mode = rt::NavigationPresentationMode::Override,
                                 .spec = resolveListAuthoringPresentation(_listPresentations, *res, expression)};

    auto const navigatedRes = _trackList.navigateTo(*res, std::move(presentation));

    if (!token.accepts())
    {
      return;
    }

    if (!navigatedRes)
    {
      reportWorkflowError(navigatedRes.error());
    }
    else if (_reportStatus)
    {
      _reportStatus(
        i18n::requiredFormat(_textCatalog, i18n::MessageId::WinUiListStatus, {i18n::MessageArgument{"list", name}}));
    }

    if (token.accepts())
    {
      _dialog.Hide();
    }
  }

  void ListAuthoringCoordinator::deleteList(ListId const listId, bool const includeDescendants)
  {
    if (_retired || _dialogActive || rt::isVirtualListId(listId))
    {
      return;
    }

    beginDialogWorkflow();
    _dialogActive = true;
    auto submission = uimodel::previewListDeletionAsync(_editing, listId, includeDescendants);
    AO_INVARIANT(
      _optDialogTasks && _dialogGenerationGate.isOpen(), "A List deletion preview requires an active dialog workflow");
    auto const token = _dialogGenerationGate.token();
    _asyncRuntime.spawnWithLifetime(
      *_optDialogTasks,
      [runtime = &_asyncRuntime, owner = this, token, listId, includeDescendants, submission = std::move(submission)](
        std::stop_token const stopToken) mutable
      {
        return finishOnCallbackExecutorAsync(
          runtime,
          owner,
          token,
          std::move(submission),
          [listId, includeDescendants](ListAuthoringCoordinator* target, Result<rt::DeleteListSubtreeReply> res)
          { target->finishDeletePreview(listId, includeDescendants, std::move(res)); },
          stopToken);
      },
      "Windows List deletion preview");
  }

  void ListAuthoringCoordinator::finishDeletePreview(ListId const listId,
                                                     bool const includeDescendants,
                                                     Result<rt::DeleteListSubtreeReply> res)
  {
    if (res && res->deletedLists.empty())
    {
      res = makeError(Error::Code::InvalidState, "The list deletion preview returned no lists");
    }

    if (!res)
    {
      _dialogActive = false;
      _dialogGenerationGate.retire();

      reportWorkflowError(res.error());
      return;
    }

    buildDeleteDialog(listId, includeDescendants, *res);
    showDialog();
  }

  void ListAuthoringCoordinator::buildDeleteDialog(ListId const listId,
                                                   bool const includeDescendants,
                                                   rt::DeleteListSubtreeReply const& preview)
  {
    _deleteListId = listId;
    _deleteDescendants = includeDescendants;
    _deleteContainsActive = std::ranges::any_of(preview.deletedLists,
                                                [this](rt::DeleteListReply const& entry)
                                                { return entry.listId == _trackList.activeListId(); });
    _submitting = false;
    auto const description = uimodel::describeListDeletion(_textCatalog, preview, includeDescendants);

    _dialog = ContentDialog{};
    _dialog.XamlRoot(_xamlRoot ? _xamlRoot() : XamlRoot{nullptr});
    _dialog.Title(winrt::box_value(winrt::to_hstring(i18n::requiredText(_textCatalog, description.labels.title))));
    _dialog.PrimaryButtonText(winrt::to_hstring(i18n::requiredText(_textCatalog, description.labels.action)));
    _dialog.CloseButtonText(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::CommonCancel)));
    _dialog.DefaultButton(ContentDialogButton::Close);

    auto content = StackPanel{};
    content.Spacing(kSectionSpacing);
    _errorText = makeWrappedText(kSupportingTextOpacity);
    _errorText.Visibility(Visibility::Collapsed);
    content.Children().Append(_errorText);

    auto messageText = makeWrappedText();
    messageText.Text(winrt::to_hstring(description.question));
    content.Children().Append(messageText);

    _removeTagCheck = nullptr;

    if (auto const& optCleanup = description.optCleanup; optCleanup)
    {
      _removeTagCheck = CheckBox{};
      _removeTagCheck.Content(winrt::box_value(winrt::to_hstring(optCleanup->question)));
      content.Children().Append(_removeTagCheck);

      if (!optCleanup->referencesWarning.empty())
      {
        auto warning = makeWrappedText(kSupportingTextOpacity);
        warning.Text(winrt::to_hstring(optCleanup->referencesWarning));
        content.Children().Append(warning);
      }
    }

    _dialog.Content(content);
    _primaryClickRevoker = _dialog.PrimaryButtonClick(
      winrt::auto_revoke,
      [this](ContentDialog const&, ContentDialogButtonClickEventArgs const& args) { handleDeleteCommit(args); });
    _closedRevoker = _dialog.Closed(
      winrt::auto_revoke, [this](ContentDialog const&, ContentDialogClosedEventArgs const&) { handleDialogClosed(); });
  }

  void ListAuthoringCoordinator::handleDeleteCommit(ContentDialogButtonClickEventArgs const& args)
  {
    args.Cancel(true);

    if (_submitting || !_optDialogTasks || !_dialogGenerationGate.isOpen())
    {
      return;
    }

    _submitting = true;
    _dialog.IsPrimaryButtonEnabled(false);
    auto const removeTag = _removeTagCheck && _removeTagCheck.IsChecked().GetBoolean();
    auto submission = uimodel::deleteListAsync(
      _editing, _deleteListId, _deleteDescendants, rt::DeleteListOptions{.removeWritableTagFromTracks = removeTag});

    if (!_optDialogTasks || !_dialogGenerationGate.isOpen())
    {
      return;
    }

    auto const token = _dialogGenerationGate.token();
    _asyncRuntime.spawnWithLifetime(
      *_optDialogTasks,
      [runtime = &_asyncRuntime, owner = this, token, submission = std::move(submission)](
        std::stop_token const stopToken) mutable
      {
        return finishOnCallbackExecutorAsync(
          runtime,
          owner,
          token,
          std::move(submission),
          [](ListAuthoringCoordinator* target, Result<rt::DeleteListSubtreeReply> res)
          { target->finishDeleteCommit(std::move(res)); },
          stopToken);
      },
      "Windows List deletion");
  }

  void ListAuthoringCoordinator::finishDeleteCommit(Result<rt::DeleteListSubtreeReply> res)
  {
    _submitting = false;

    if (!res)
    {
      _dialog.IsPrimaryButtonEnabled(true);
      setDialogError(res.error().message);
      return;
    }

    auto const token = _dialogGenerationGate.token();

    if (_deleteContainsActive)
    {
      if (auto const navigatedRes = _trackList.navigateTo(rt::kAllTracksListId); token.accepts() && !navigatedRes)
      {
        reportWorkflowError(navigatedRes.error());
      }
    }

    if (token.accepts())
    {
      _dialog.Hide();
    }
  }

  std::vector<uimodel::WritableTagListTarget> ListAuthoringCoordinator::membershipTargets() const
  {
    return uimodel::writableTagListTargets(_editing.library().snapshot().lists(), _textOrderingPolicy);
  }

  void ListAuthoringCoordinator::editMembership(ListId const listId, bool const add)
  {
    if (_retired)
    {
      return;
    }

    auto stateRes = _views.findTrackListState(_trackList.viewId());

    if (!stateRes || stateRes->selection.empty())
    {
      return;
    }

    auto sessionRes = uimodel::ListMembershipAuthoringSession::begin(_editing, stateRes->selection);

    if (!sessionRes)
    {
      finishMembership(std::unexpected{std::move(sessionRes.error())});
      return;
    }

    auto session = std::move(*sessionRes);
    auto submission = add ? session.addToListAsync(listId) : session.removeFromListAsync(listId);
    auto const token = _ownerCallbackGate.token();
    _asyncRuntime.spawnWithLifetime(
      _commandTasks,
      [runtime = &_asyncRuntime, owner = this, token, submission = std::move(submission)](
        std::stop_token const stopToken) mutable
      {
        return finishOnCallbackExecutorAsync(
          runtime,
          owner,
          token,
          std::move(submission),
          [](ListAuthoringCoordinator* target, Result<uimodel::ListMembershipEditResult> res)
          { target->finishMembership(std::move(res)); },
          stopToken);
      },
      "Windows List membership edit");
  }

  void ListAuthoringCoordinator::finishMembership(Result<uimodel::ListMembershipEditResult> res)
  {
    if (!res)
    {
      reportWorkflowError(res.error());
      return;
    }

    auto notification = uimodel::listMembershipEditNotification(_textCatalog, *res);
    reportFeedback(notification.severity, std::move(notification.text), notification.lifetime);
  }

  void ListAuthoringCoordinator::reportFeedback(rt::NotificationSeverity const severity,
                                                std::string text,
                                                rt::NotificationLifetime const lifetime)
  {
    // Feed observers run synchronously and can retire this owner.
    auto const token = _ownerCallbackGate.token();
    _notifications.post(severity, text, lifetime);

    if (token.accepts() && _reportStatus)
    {
      _reportStatus(std::move(text));
    }
  }

  void ListAuthoringCoordinator::reportWorkflowError(Error const& error)
  {
    reportFeedback(rt::NotificationSeverity::Error,
                   i18n::requiredFormat(_textCatalog, i18n::MessageId::WinUiError, {{"detail", error.message}}),
                   rt::NotificationLifetime::history());
  }

  uimodel::ListOrderCapabilityState ListAuthoringCoordinator::orderCapabilities() const
  {
    auto const stateRes = _views.findTrackListState(_trackList.viewId());

    if (!stateRes)
    {
      return uimodel::ListOrderCapabilityState{
        .disabledReason = std::string{i18n::requiredText(_textCatalog, i18n::MessageId::ListOrderListUnavailable)},
      };
    }

    auto const sourceStateRes = _views.listSourceState(_trackList.viewId());
    return uimodel::describeViewListOrderCapabilities(_textCatalog,
                                                      *stateRes,
                                                      sourceStateRes.value_or(rt::TrackSourceState::Invalidated),
                                                      _editing.library().authoringAvailability());
  }

  void ListAuthoringCoordinator::applyOrder(ListOrderCommand const command)
  {
    if (_retired)
    {
      return;
    }

    auto sessionRes = uimodel::ListOrderAuthoringSession::begin(_editing, _views, _trackList.viewId(), _textCatalog);

    if (!sessionRes)
    {
      reportFeedback(rt::NotificationSeverity::Info, sessionRes.error().message, rt::NotificationLifetime::transient());
      return;
    }

    auto stateRes = _views.findTrackListState(_trackList.viewId());
    auto selected = stateRes ? std::move(stateRes->selection) : std::vector<TrackId>{};
    auto session = std::move(*sessionRes);
    auto const token = _ownerCallbackGate.token();

    if (command == ListOrderCommand::Reset)
    {
      auto submission = session.resetOrderAsync();
      _asyncRuntime.spawnWithLifetime(
        _commandTasks,
        [runtime = &_asyncRuntime, owner = this, token, submission = std::move(submission)](
          std::stop_token const stopToken) mutable
        {
          return finishOnCallbackExecutorAsync(
            runtime,
            owner,
            token,
            std::move(submission),
            [](ListAuthoringCoordinator* target, Result<rt::AuthoringResult<rt::ResetListOrderReply>> res)
            { target->finishOrder(std::move(res)); },
            stopToken);
        },
        "Windows List order reset");
      return;
    }

    auto submission = [&]
    {
      switch (command)
      {
        case ListOrderCommand::MoveUp: return session.moveUpAsync(std::move(selected));
        case ListOrderCommand::MoveDown: return session.moveDownAsync(std::move(selected));
        case ListOrderCommand::MoveToTop: return session.moveToTopAsync(std::move(selected));
        case ListOrderCommand::MoveToBottom: return session.moveToBottomAsync(std::move(selected));
        case ListOrderCommand::Reset: break;
      }

      return session.moveUpAsync({});
    }();
    _asyncRuntime.spawnWithLifetime(
      _commandTasks,
      [runtime = &_asyncRuntime, owner = this, token, submission = std::move(submission)](
        std::stop_token const stopToken) mutable
      {
        return finishOnCallbackExecutorAsync(
          runtime,
          owner,
          token,
          std::move(submission),
          [](ListAuthoringCoordinator* target, Result<rt::AuthoringResult<rt::MoveListOrderReply>> res)
          { target->finishOrder(std::move(res)); },
          stopToken);
      },
      "Windows List order edit");
  }

  template<typename Reply>
  void ListAuthoringCoordinator::finishOrder(Result<rt::AuthoringResult<Reply>> res)
  {
    if (!res)
    {
      reportFeedback(rt::NotificationSeverity::Warning, res.error().message, rt::NotificationLifetime::history());
      return;
    }

    reportFeedback(rt::NotificationSeverity::Info,
                   uimodel::formatListOrderOutcome(_textCatalog, *res),
                   rt::NotificationLifetime::transient());
  }

  void ListAuthoringCoordinator::beginDialogWorkflow()
  {
    // Close the old generation before cancellation can publish a completion.
    // The gate admits callbacks only; task settlement keeps this owner alive.
    _dialogGenerationGate.retire();

    if (_optDialogTasks)
    {
      _optDialogTasks->cancelAll();
      _optDialogTasks.reset();
    }

    // LifetimeScope cancellation is terminal. Each independently presented
    // dialog therefore needs a fresh scope and callback generation.
    _optDialogTasks.emplace();
    _dialogGenerationGate.renew();
  }

  void ListAuthoringCoordinator::showDialog()
  {
    _dialogActive = true;
    AO_INVARIANT(_optDialogTasks && _dialogGenerationGate.isOpen(),
                 "A List dialog requires an active task scope and callback generation");

    try
    {
      _showOperation = _dialog.ShowAsync();
    }
    catch (...)
    {
      handleDialogClosed();
      throw;
    }
  }

  void ListAuthoringCoordinator::setDialogError(std::string text)
  {
    if (_errorText)
    {
      _errorText.Text(winrt::to_hstring(text));
      _errorText.Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
    }
  }

  void ListAuthoringCoordinator::handleDialogClosed()
  {
    _dialogActive = false;
    _submitting = false;
    _dialogGenerationGate.retire();

    if (_optDialogTasks)
    {
      _optDialogTasks->cancelAll();
      _optDialogTasks.reset();
    }

    if (_previewTimer)
    {
      _previewTimer.Stop();
    }

    _primaryClickRevoker.revoke();
    _closedRevoker.revoke();
    _nameChangedRevoker.revoke();
    _descriptionChangedRevoker.revoke();
    _filterChangedRevoker.revoke();
    _previewTickRevoker.revoke();
    _previewChanges.reset();
    _optPreviewLease.reset();
    _showOperation = nullptr;
    _dialog = nullptr;
    _nameInput = nullptr;
    _descriptionInput = nullptr;
    _filterInput = nullptr;
    _presentationInput = nullptr;
    _inheritedFilterText = nullptr;
    _effectiveFilterText = nullptr;
    _membershipText = nullptr;
    _previewText = nullptr;
    _errorText = nullptr;
    _removeTagCheck = nullptr;
    _previewTimer = nullptr;
  }

  void ListAuthoringCoordinator::retire() noexcept
  {
    if (_retired)
    {
      return;
    }

    _retired = true;
    _ownerCallbackGate.retire();
    _dialogGenerationGate.retire();

    if (_optDialogTasks)
    {
      _optDialogTasks->cancelAll();
      _optDialogTasks.reset();
    }

    _commandTasks.cancelAll();

    if (_dialog)
    {
      // Callback admission is already closed, so native dismissal is
      // presentation-only cleanup rather than an operation invariant.
      runOptionalWinRt("hiding the WinUI list-authoring dialog", [this] { _dialog.Hide(); });
    }

    handleDialogClosed();
  }
} // namespace ao::winui

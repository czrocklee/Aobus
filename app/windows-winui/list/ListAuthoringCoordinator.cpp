// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "list/ListAuthoringCoordinator.h"

#include "pch.h"
#include "track/TrackListController.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/library/list/ListMembershipAuthoringSession.h>
#include <ao/uimodel/library/list/ListOrderAuthoringSession.h>
#include <ao/uimodel/library/list/ListOrderPolicy.h>
#include <ao/uimodel/library/list/SmartListEditorModel.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
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

    constexpr auto kPreviewDebounceInterval = std::chrono::milliseconds{100};
    constexpr double kEditorMinWidth = 620.0;
    constexpr double kDialogMaxHeight = 680.0;
    constexpr double kSectionSpacing = 12.0;
    constexpr double kSupportingTextOpacity = 0.82;
    constexpr double kContextTextOpacity = 0.72;
    constexpr double kPreviewHeadingFontSize = 18.0;

    std::string displayedTag(std::string_view const tag)
    {
      return query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = std::string{tag}});
    }

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

    std::size_t affectedTrackCount(rt::MoveListOrderReply const& reply) noexcept
    {
      return reply.selectedTrackIds.size();
    }

    void appendReferences(std::string& target, std::span<rt::DeleteListReply::TagReference const> const references)
    {
      for (auto const& reference : references)
      {
        if (!target.empty())
        {
          target.append(", ");
        }

        target.append(reference.name);
      }
    }
  } // namespace

  ListAuthoringCoordinator::ListAuthoringCoordinator(ListAuthoringCoordinatorConfig config)
    : _xamlRoot{std::move(config.xamlRoot)}
    , _asyncRuntime{config.asyncRuntime}
    , _library{config.library}
    , _views{config.views}
    , _sources{config.sources}
    , _trackList{config.trackList}
    , _presentationCatalog{config.presentationCatalog}
    , _presentationPreferences{config.presentationPreferences}
    , _textOrderingPolicy{config.textOrderingPolicy}
    , _textCatalog{std::move(config.textCatalog)}
    , _reportStatus{std::move(config.reportStatus)}
    , _callbackLifetimePtr{std::make_shared<std::monostate>()}
  {
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

    auto const optNode = _library.reader().listNode(listId);

    if (!optNode)
    {
      setDialogError(std::string{_textCatalog.text(i18n::MessageId::ListOrderListUnavailable)});
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
    _inheritedExpression.clear();

    if (!rt::isVirtualListId(parentListId))
    {
      if (auto const optParent = _library.reader().listNode(parentListId); optParent)
      {
        _inheritedExpression = optParent->expression;
      }
    }

    buildEditorDialog();
    _nameInput.Text(winrt::to_hstring(name));
    _descriptionInput.Text(winrt::to_hstring(description));
    _filterInput.Text(winrt::to_hstring(expression));

    std::int32_t selectedPresentationIndex = 0;

    if (auto const optId = _presentationPreferences.presentationIdForList(editListId); optId)
    {
      auto const selected = std::ranges::find(_presentationIds, *optId);

      if (selected != _presentationIds.end())
      {
        selectedPresentationIndex = static_cast<std::int32_t>(std::distance(_presentationIds.begin(), selected));
      }
    }

    _presentationInput.SelectedIndex(selectedPresentationIndex);
    updateEditorPreview();
    showDialog();
  }

  void ListAuthoringCoordinator::buildEditorDialog()
  {
    _dialog = ContentDialog{};
    _dialog.XamlRoot(_xamlRoot ? _xamlRoot() : XamlRoot{nullptr});
    _dialog.MinWidth(kEditorMinWidth);
    _dialog.Title(winrt::box_value(
      winrt::to_hstring(_textCatalog.text(_editListId == kInvalidListId ? i18n::MessageId::WinUiListEditorNewTitle
                                                                        : i18n::MessageId::WinUiListEditorEditTitle))));
    _dialog.PrimaryButtonText(winrt::to_hstring(_textCatalog.text(
      _editListId == kInvalidListId ? i18n::MessageId::WinUiCommonCreate : i18n::MessageId::WinUiCommonSave)));
    _dialog.CloseButtonText(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiCommonCancel)));
    _dialog.DefaultButton(ContentDialogButton::Primary);

    auto content = StackPanel{};
    content.Spacing(kSectionSpacing);

    _errorText = makeWrappedText(kSupportingTextOpacity);
    _errorText.Visibility(Visibility::Collapsed);
    content.Children().Append(_errorText);

    _nameInput = TextBox{};
    _nameInput.Header(winrt::box_value(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiListName))));
    _nameInput.PlaceholderText(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiListNamePlaceholder)));
    content.Children().Append(_nameInput);

    _descriptionInput = TextBox{};
    _descriptionInput.Header(
      winrt::box_value(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiListDescription))));
    _descriptionInput.PlaceholderText(
      winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiListDescriptionPlaceholder)));
    content.Children().Append(_descriptionInput);

    _inheritedFilterText = makeWrappedText(kContextTextOpacity);
    content.Children().Append(_inheritedFilterText);

    _filterInput = TextBox{};
    _filterInput.Header(winrt::box_value(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiListLocalFilter))));
    _filterInput.PlaceholderText(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiListFilterPlaceholder)));
    content.Children().Append(_filterInput);

    _effectiveFilterText = makeWrappedText(kContextTextOpacity);
    _membershipText = makeWrappedText(kContextTextOpacity);
    content.Children().Append(_effectiveFilterText);
    content.Children().Append(_membershipText);

    _presentationInput = ComboBox{};
    _presentationInput.Header(
      winrt::box_value(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiListPresentation))));
    _presentationInput.HorizontalAlignment(HorizontalAlignment::Stretch);
    _presentationIds.clear();
    _presentationIds.emplace_back();
    _presentationInput.Items().Append(
      winrt::box_value(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiListAutoPresentation))));

    for (auto const& item : _presentationCatalog.menuItems())
    {
      if (item.type != uimodel::TrackPresentationMenuItemType::Preset)
      {
        continue;
      }

      _presentationIds.push_back(item.id);
      _presentationInput.Items().Append(winrt::box_value(winrt::to_hstring(item.label)));
    }

    content.Children().Append(_presentationInput);

    auto previewHeading = TextBlock{};
    previewHeading.Text(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiListPreview)));
    previewHeading.FontSize(kPreviewHeadingFontSize);
    content.Children().Append(previewHeading);
    _previewText = makeWrappedText(kSupportingTextOpacity);
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
    _previewTickRevoker = _previewTimer.Tick(winrt::auto_revoke,
                                             [this](winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const&,
                                                    IInspectable const&) { updateEditorPreview(); });
    _nameChangedRevoker = _nameInput.TextChanged(winrt::auto_revoke,
                                                 [this](IInspectable const&, TextChangedEventArgs const&)
                                                 {
                                                   _mutationError.clear();
                                                   updateEditorPreview();
                                                 });
    _filterChangedRevoker = _filterInput.TextChanged(winrt::auto_revoke,
                                                     [this](IInspectable const&, TextChangedEventArgs const&)
                                                     {
                                                       _mutationError.clear();
                                                       scheduleEditorPreview();
                                                     });
    _primaryClickRevoker = _dialog.PrimaryButtonClick(
      winrt::auto_revoke,
      [this](ContentDialog const&, ContentDialogButtonClickEventArgs const& args) { handleEditorSave(args); });
    _closedRevoker = _dialog.Closed(
      winrt::auto_revoke, [this](ContentDialog const&, ContentDialogClosedEventArgs const&) { handleDialogClosed(); });
  }

  void ListAuthoringCoordinator::scheduleEditorPreview()
  {
    if (!_previewTimer)
    {
      return;
    }

    _previewTimer.Stop();
    _previewTimer.Interval(kPreviewDebounceInterval);
    _previewTimer.Start();
  }

  void ListAuthoringCoordinator::updateEditorPreview()
  {
    if (!_dialog || !_nameInput || !_filterInput)
    {
      return;
    }

    if (_previewTimer)
    {
      _previewTimer.Stop();
    }

    auto const expression = winrt::to_string(_filterInput.Text());
    auto const sourceListId = rt::resolveParentSourceId(_parentListId);
    auto previewRes = _sources.acquire(rt::SourceSpec{.baseListId = sourceListId, .filterExpression = expression});
    auto optPreviewError = std::optional<Error>{};
    auto input = uimodel::SmartListPreviewState{
      .name = winrt::to_string(_nameInput.Text()),
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
    }

    _editorState = uimodel::makeSmartListEditorViewState(_textCatalog, input);
    auto const inherited = uimodel::formatSmartListExpressionDisplayText(_textCatalog, _inheritedExpression);
    auto const effective = uimodel::formatSmartListExpressionDisplayText(
      _textCatalog, uimodel::combineSmartListEffectiveExpression(_inheritedExpression, expression));
    _inheritedFilterText.Text(
      winrt::to_hstring(labeled(_textCatalog.text(i18n::MessageId::WinUiListInheritedFilter), inherited)));
    _effectiveFilterText.Text(
      winrt::to_hstring(labeled(_textCatalog.text(i18n::MessageId::WinUiListEffectiveFilter), effective)));
    _membershipText.Text(winrt::to_hstring(
      labeled(_textCatalog.text(i18n::MessageId::WinUiListMembership), _editorState.membershipEditingText)));
    _previewText.Text(winrt::to_hstring(_editorState.previewStatusText));

    auto error = _mutationError;

    if (error.empty() && !previewRes)
    {
      error = previewRes.error().message;
    }
    else if (error.empty() && _editorState.errorVisible)
    {
      error = _editorState.errorText;
    }

    setDialogError(std::move(error));
    _dialog.IsPrimaryButtonEnabled(_editorState.canSubmit && !_submitting);
  }

  void ListAuthoringCoordinator::handleEditorSave(ContentDialogButtonClickEventArgs const& args)
  {
    args.Cancel(true);

    if (_submitting || !_editorState.canSubmit)
    {
      return;
    }

    _submitting = true;
    _mutationError.clear();
    updateEditorPreview();
    auto draft = uimodel::makeSmartListDraft(_parentListId,
                                             _editListId,
                                             winrt::to_string(_nameInput.Text()),
                                             winrt::to_string(_descriptionInput.Text()),
                                             winrt::to_string(_filterInput.Text()));
    auto submission = writeListDraft(&_library, std::move(draft));
    auto lifetimePtr = std::weak_ptr<std::monostate>{_dialogLifetimePtr};
    _asyncRuntime.spawnWithLifetime(
      *_dialogTasksPtr,
      [runtime = &_asyncRuntime, owner = this, lifetimePtr, submission = std::move(submission)](
        std::stop_token const stopToken) mutable
      {
        return finishOnCallbackExecutor(
          runtime,
          owner,
          lifetimePtr,
          std::move(submission),
          [](ListAuthoringCoordinator* target, Result<ListId> result) { target->finishEditorSave(std::move(result)); },
          stopToken);
      },
      "Windows List save");
  }

  async::Task<Result<ListId>> ListAuthoringCoordinator::writeListDraft(rt::Library* const library, rt::ListDraft draft)
  {
    if (draft.listId == kInvalidListId)
    {
      co_return co_await library->createList(std::move(draft));
    }

    auto const listId = draft.listId;

    if (auto result = co_await library->updateList(std::move(draft)); !result)
    {
      co_return std::unexpected{result.error()};
    }

    co_return listId;
  }

  void ListAuthoringCoordinator::finishEditorSave(Result<ListId> result)
  {
    _submitting = false;

    if (!result)
    {
      _mutationError = result.error().message;
      updateEditorPreview();
      return;
    }

    auto const selected = _presentationInput ? _presentationInput.SelectedIndex() : -1;

    if (selected > 0 && static_cast<std::size_t>(selected) < _presentationIds.size())
    {
      _presentationPreferences.setPresentationIdForList(*result, _presentationIds[static_cast<std::size_t>(selected)]);
    }
    else
    {
      _presentationPreferences.clearPresentationForList(*result);
    }

    auto const name = _nameInput ? winrt::to_string(_nameInput.Text()) : std::string{};
    auto const expression = _filterInput ? winrt::to_string(_filterInput.Text()) : std::string{};
    auto const presentation = resolveListAuthoringPresentation(_presentationPreferences, *result, expression);

    if (auto const navigatedRes = _trackList.navigateTo(*result); !navigatedRes)
    {
      if (_reportStatus)
      {
        _reportStatus(_textCatalog.format(
          i18n::MessageId::WinUiError, {i18n::MessageArgument{"detail", navigatedRes.error().message}}));
      }
    }
    else if (auto const selectedRes = _trackList.selectPresentation(presentation); !selectedRes)
    {
      if (_reportStatus)
      {
        _reportStatus(_textCatalog.format(
          i18n::MessageId::WinUiPresentationFailed, {i18n::MessageArgument{"detail", selectedRes.error().message}}));
      }
    }
    else if (_reportStatus)
    {
      _reportStatus(_textCatalog.format(i18n::MessageId::WinUiListStatus, {i18n::MessageArgument{"list", name}}));
    }

    if (_dialog)
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
    _dialogLifetimePtr = std::make_shared<std::monostate>();
    auto submission = previewDelete(&_library, listId, includeDescendants);
    auto lifetimePtr = std::weak_ptr<std::monostate>{_dialogLifetimePtr};
    _asyncRuntime.spawnWithLifetime(
      *_dialogTasksPtr,
      [runtime = &_asyncRuntime,
       owner = this,
       lifetimePtr,
       listId,
       includeDescendants,
       submission = std::move(submission)](std::stop_token const stopToken) mutable
      {
        return finishOnCallbackExecutor(
          runtime,
          owner,
          lifetimePtr,
          std::move(submission),
          [listId, includeDescendants](ListAuthoringCoordinator* target, Result<rt::DeleteListSubtreeReply> result)
          { target->finishDeletePreview(listId, includeDescendants, std::move(result)); },
          stopToken);
      },
      "Windows List deletion preview");
  }

  async::Task<Result<rt::DeleteListSubtreeReply>> ListAuthoringCoordinator::previewDelete(rt::Library* const library,
                                                                                          ListId const listId,
                                                                                          bool const includeDescendants)
  {
    if (includeDescendants)
    {
      co_return co_await library->previewDeleteListAndDescendants(listId);
    }

    auto result = co_await library->previewDeleteList(listId);

    if (!result)
    {
      co_return std::unexpected{result.error()};
    }

    co_return rt::DeleteListSubtreeReply{.rootListId = listId, .deletedLists = {std::move(*result)}};
  }

  void ListAuthoringCoordinator::finishDeletePreview(ListId const listId,
                                                     bool const includeDescendants,
                                                     Result<rt::DeleteListSubtreeReply> result)
  {
    if (result && result->deletedLists.empty())
    {
      result = makeError(Error::Code::InvalidState, "The list deletion preview returned no lists");
    }

    if (!result)
    {
      _dialogActive = false;
      _dialogLifetimePtr.reset();

      if (_reportStatus)
      {
        _reportStatus(
          _textCatalog.format(i18n::MessageId::WinUiError, {i18n::MessageArgument{"detail", result.error().message}}));
      }

      return;
    }

    buildDeleteDialog(listId, includeDescendants, *result);
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

    _dialog = ContentDialog{};
    _dialog.XamlRoot(_xamlRoot ? _xamlRoot() : XamlRoot{nullptr});
    _dialog.Title(winrt::box_value(winrt::to_hstring(_textCatalog.text(
      includeDescendants ? i18n::MessageId::WinUiListDeleteSubtreeTitle : i18n::MessageId::WinUiListDeleteTitle))));
    _dialog.PrimaryButtonText(winrt::to_hstring(_textCatalog.text(
      includeDescendants ? i18n::MessageId::WinUiListDeleteAllCommit : i18n::MessageId::WinUiListDeleteCommit)));
    _dialog.CloseButtonText(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiCommonCancel)));
    _dialog.DefaultButton(ContentDialogButton::Close);

    auto content = StackPanel{};
    content.Spacing(kSectionSpacing);
    _errorText = makeWrappedText(kSupportingTextOpacity);
    _errorText.Visibility(Visibility::Collapsed);
    content.Children().Append(_errorText);

    auto entries = std::string{};

    for (auto const& entry : preview.deletedLists)
    {
      entries.append(std::format("• {} ({})\n", entry.name, entry.listId.raw()));
    }

    auto const message = includeDescendants
                           ? _textCatalog.format(i18n::MessageId::WinUiListDeleteSubtreeQuestion,
                                                 {i18n::MessageArgument{"count", preview.deletedLists.size()},
                                                  i18n::MessageArgument{"entries", entries}})
                           : _textCatalog.format(i18n::MessageId::WinUiListDeleteQuestion,
                                                 {i18n::MessageArgument{"name", preview.deletedLists.front().name}});
    auto messageText = makeWrappedText();
    messageText.Text(winrt::to_hstring(message));
    content.Children().Append(messageText);

    auto const optTagImpact = preview.deletedLists.front().optTagImpact;
    _removeTagCheck = nullptr;

    if (optTagImpact)
    {
      auto const tag = displayedTag(optTagImpact->tag);
      _removeTagCheck = CheckBox{};
      _removeTagCheck.Content(winrt::box_value(winrt::to_hstring(_textCatalog.format(
        i18n::MessageId::WinUiListRemoveTag,
        {i18n::MessageArgument{"tag", tag}, i18n::MessageArgument{"count", optTagImpact->taggedTrackCount}}))));
      content.Children().Append(_removeTagCheck);

      if (!optTagImpact->otherListReferences.empty())
      {
        auto references = std::string{};
        appendReferences(references, optTagImpact->otherListReferences);
        auto warning = makeWrappedText(kSupportingTextOpacity);
        warning.Text(winrt::to_hstring(
          _textCatalog.format(i18n::MessageId::WinUiListTagReferences,
                              {i18n::MessageArgument{"tag", tag}, i18n::MessageArgument{"references", references}})));
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

    if (_submitting)
    {
      return;
    }

    _submitting = true;
    _dialog.IsPrimaryButtonEnabled(false);
    auto const removeTag = _removeTagCheck && _removeTagCheck.IsChecked().GetBoolean();
    auto submission = commitDelete(
      &_library, _deleteListId, _deleteDescendants, rt::DeleteListOptions{.removeWritableTagFromTracks = removeTag});
    auto lifetimePtr = std::weak_ptr<std::monostate>{_dialogLifetimePtr};
    _asyncRuntime.spawnWithLifetime(
      *_dialogTasksPtr,
      [runtime = &_asyncRuntime, owner = this, lifetimePtr, submission = std::move(submission)](
        std::stop_token const stopToken) mutable
      {
        return finishOnCallbackExecutor(
          runtime,
          owner,
          lifetimePtr,
          std::move(submission),
          [](ListAuthoringCoordinator* target, Result<rt::DeleteListSubtreeReply> result)
          { target->finishDeleteCommit(std::move(result)); },
          stopToken);
      },
      "Windows List deletion");
  }

  async::Task<Result<rt::DeleteListSubtreeReply>> ListAuthoringCoordinator::commitDelete(
    rt::Library* const library,
    ListId const listId,
    bool const includeDescendants,
    rt::DeleteListOptions const options)
  {
    if (includeDescendants)
    {
      co_return co_await library->deleteListAndDescendants(listId, options);
    }

    auto result = co_await library->deleteList(listId, options);

    if (!result)
    {
      co_return std::unexpected{result.error()};
    }

    co_return rt::DeleteListSubtreeReply{.rootListId = listId, .deletedLists = {std::move(*result)}};
  }

  void ListAuthoringCoordinator::finishDeleteCommit(Result<rt::DeleteListSubtreeReply> result)
  {
    _submitting = false;

    if (!result)
    {
      _dialog.IsPrimaryButtonEnabled(true);
      setDialogError(result.error().message);
      return;
    }

    if (_deleteContainsActive)
    {
      if (auto const navigatedRes = _trackList.navigateTo(rt::kAllTracksListId); !navigatedRes && _reportStatus)
      {
        _reportStatus(_textCatalog.format(
          i18n::MessageId::WinUiError, {i18n::MessageArgument{"detail", navigatedRes.error().message}}));
      }
    }

    if (_dialog)
    {
      _dialog.Hide();
    }
  }

  std::vector<uimodel::WritableTagListTarget> ListAuthoringCoordinator::membershipTargets() const
  {
    return uimodel::writableTagListTargets(_library.reader().lists(), _textOrderingPolicy);
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

    auto sessionRes = uimodel::ListMembershipAuthoringSession::begin(_library, stateRes->selection, _textCatalog);

    if (!sessionRes)
    {
      if (_reportStatus)
      {
        _reportStatus(_textCatalog.format(
          i18n::MessageId::WinUiError, {i18n::MessageArgument{"detail", sessionRes.error().message}}));
      }

      return;
    }

    auto sessionPtr = std::shared_ptr<uimodel::ListMembershipAuthoringSession>{std::move(*sessionRes)};
    auto submission = add ? sessionPtr->addToList(listId) : sessionPtr->removeFromList(listId);
    auto lifetimePtr = std::weak_ptr<std::monostate>{_callbackLifetimePtr};
    _asyncRuntime.spawnWithLifetime(
      _commandTasks,
      [runtime = &_asyncRuntime,
       owner = this,
       lifetimePtr,
       sessionPtr = std::move(sessionPtr),
       submission = std::move(submission)](std::stop_token const stopToken) mutable
      {
        return finishOnCallbackExecutor(
          runtime,
          owner,
          lifetimePtr,
          std::move(submission),
          [](ListAuthoringCoordinator* target, Result<uimodel::ListMembershipEditResult> result)
          { target->finishMembership(std::move(result)); },
          stopToken);
      },
      "Windows List membership edit");
  }

  void ListAuthoringCoordinator::finishMembership(Result<uimodel::ListMembershipEditResult> result)
  {
    if (!_reportStatus)
    {
      return;
    }

    if (!result)
    {
      _reportStatus(
        _textCatalog.format(i18n::MessageId::WinUiError, {i18n::MessageArgument{"detail", result.error().message}}));
      return;
    }

    _reportStatus(result->notificationText);
  }

  uimodel::ListOrderCapabilityState ListAuthoringCoordinator::orderCapabilities() const
  {
    auto const stateRes = _views.findTrackListState(_trackList.viewId());

    if (!stateRes)
    {
      return uimodel::ListOrderCapabilityState{
        .disabledReason = std::string{_textCatalog.text(i18n::MessageId::ListOrderListUnavailable)},
      };
    }

    auto const sourceStateRes = _views.listSourceState(_trackList.viewId());
    return uimodel::describeListOrderCapabilities(
      _textCatalog,
      uimodel::ListOrderCapabilityInput{
        .listId = stateRes->listId,
        .presentation = stateRes->presentation,
        .quickFilterExpression = stateRes->filterExpression,
        .sourceLive = sourceStateRes && *sourceStateRes == rt::TrackSourceState::Live,
        .sourceHasError = stateRes->optFilterError.has_value(),
        .authoring = _library.authoringAvailability(),
      });
  }

  void ListAuthoringCoordinator::applyOrder(ListOrderCommand const command)
  {
    if (_retired)
    {
      return;
    }

    auto sessionRes = uimodel::ListOrderAuthoringSession::begin(_library, _views, _trackList.viewId(), _textCatalog);

    if (!sessionRes)
    {
      if (_reportStatus)
      {
        _reportStatus(sessionRes.error().message);
      }

      return;
    }

    auto stateRes = _views.findTrackListState(_trackList.viewId());
    auto selected = stateRes ? std::move(stateRes->selection) : std::vector<TrackId>{};
    auto sessionPtr = std::shared_ptr<uimodel::ListOrderAuthoringSession>{std::move(*sessionRes)};
    auto lifetimePtr = std::weak_ptr<std::monostate>{_callbackLifetimePtr};

    if (command == ListOrderCommand::Reset)
    {
      auto submission = sessionPtr->resetOrder();
      _asyncRuntime.spawnWithLifetime(
        _commandTasks,
        [runtime = &_asyncRuntime,
         owner = this,
         lifetimePtr,
         sessionPtr = std::move(sessionPtr),
         submission = std::move(submission)](std::stop_token const stopToken) mutable
        {
          return finishOnCallbackExecutor(
            runtime,
            owner,
            lifetimePtr,
            std::move(submission),
            [](ListAuthoringCoordinator* target, Result<rt::AuthoringResult<rt::ResetListOrderReply>> result)
            { target->finishOrderReset(std::move(result)); },
            stopToken);
        },
        "Windows List order reset");
      return;
    }

    auto submission = [&]
    {
      switch (command)
      {
        case ListOrderCommand::MoveUp: return sessionPtr->moveUp(std::move(selected));
        case ListOrderCommand::MoveDown: return sessionPtr->moveDown(std::move(selected));
        case ListOrderCommand::MoveToTop: return sessionPtr->moveToTop(std::move(selected));
        case ListOrderCommand::MoveToBottom: return sessionPtr->moveToBottom(std::move(selected));
        case ListOrderCommand::Reset: break;
      }

      return sessionPtr->moveUp({});
    }();
    _asyncRuntime.spawnWithLifetime(
      _commandTasks,
      [runtime = &_asyncRuntime,
       owner = this,
       lifetimePtr,
       command,
       sessionPtr = std::move(sessionPtr),
       submission = std::move(submission)](std::stop_token const stopToken) mutable
      {
        return finishOnCallbackExecutor(
          runtime,
          owner,
          lifetimePtr,
          std::move(submission),
          [command](ListAuthoringCoordinator* target, Result<rt::AuthoringResult<rt::MoveListOrderReply>> result)
          { target->finishOrder(command, std::move(result)); },
          stopToken);
      },
      "Windows List order edit");
  }

  void ListAuthoringCoordinator::finishOrder(ListOrderCommand const /*command*/,
                                             Result<rt::AuthoringResult<rt::MoveListOrderReply>> result)
  {
    if (!_reportStatus)
    {
      return;
    }

    if (!result)
    {
      _reportStatus(result.error().message);
      return;
    }

    switch (result->status)
    {
      case rt::AuthoringStatus::Applied:
        _reportStatus(_textCatalog.format(
          i18n::MessageId::ListOrderMoved, {i18n::MessageArgument{"count", affectedTrackCount(result->reply)}}));
        return;
      case rt::AuthoringStatus::NoOp:
        _reportStatus(std::string{_textCatalog.text(i18n::MessageId::ListOrderUnchanged)});
        return;
      case rt::AuthoringStatus::Busy:
        _reportStatus(std::string{_textCatalog.text(i18n::MessageId::ListOrderLibraryBusy)});
        return;
      case rt::AuthoringStatus::Stale:
        _reportStatus(std::string{_textCatalog.text(i18n::MessageId::ListOrderChanged)});
        return;
      case rt::AuthoringStatus::Unavailable:
        _reportStatus(std::string{_textCatalog.text(i18n::MessageId::ListOrderEditingUnavailable)});
        return;
    }
  }

  void ListAuthoringCoordinator::finishOrderReset(Result<rt::AuthoringResult<rt::ResetListOrderReply>> result)
  {
    if (!_reportStatus)
    {
      return;
    }

    if (!result)
    {
      _reportStatus(result.error().message);
      return;
    }

    switch (result->status)
    {
      case rt::AuthoringStatus::Applied:
        _reportStatus(_textCatalog.format(
          i18n::MessageId::ListOrderReset, {i18n::MessageArgument{"count", result->reply.forgottenPositionCount}}));
        return;
      case rt::AuthoringStatus::NoOp:
        _reportStatus(std::string{_textCatalog.text(i18n::MessageId::ListOrderUnchanged)});
        return;
      case rt::AuthoringStatus::Busy:
        _reportStatus(std::string{_textCatalog.text(i18n::MessageId::ListOrderLibraryBusy)});
        return;
      case rt::AuthoringStatus::Stale:
        _reportStatus(std::string{_textCatalog.text(i18n::MessageId::ListOrderChanged)});
        return;
      case rt::AuthoringStatus::Unavailable:
        _reportStatus(std::string{_textCatalog.text(i18n::MessageId::ListOrderEditingUnavailable)});
        return;
    }
  }

  void ListAuthoringCoordinator::beginDialogWorkflow()
  {
    if (_dialogTasksPtr)
    {
      _dialogTasksPtr->cancelAll();
    }

    // LifetimeScope cancellation is terminal. Each independently presented
    // dialog therefore needs a fresh scope after the previous one closes.
    _dialogTasksPtr = std::make_unique<async::LifetimeScope>();
  }

  void ListAuthoringCoordinator::showDialog()
  {
    _dialogActive = true;

    if (!_dialogLifetimePtr)
    {
      _dialogLifetimePtr = std::make_shared<std::monostate>();
    }

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
    if (!_errorText)
    {
      if (!text.empty() && _reportStatus)
      {
        _reportStatus(std::move(text));
      }

      return;
    }

    _errorText.Text(winrt::to_hstring(text));
    _errorText.Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
  }

  void ListAuthoringCoordinator::handleDialogClosed()
  {
    _dialogActive = false;
    _submitting = false;
    _dialogLifetimePtr.reset();

    if (_dialogTasksPtr)
    {
      _dialogTasksPtr->cancelAll();
    }

    if (_previewTimer)
    {
      _previewTimer.Stop();
    }

    _primaryClickRevoker.revoke();
    _closedRevoker.revoke();
    _nameChangedRevoker.revoke();
    _filterChangedRevoker.revoke();
    _previewTickRevoker.revoke();
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
    _callbackLifetimePtr.reset();
    _dialogLifetimePtr.reset();

    if (_dialogTasksPtr)
    {
      _dialogTasksPtr->cancelAll();
    }

    _commandTasks.cancelAll();

    if (_dialog)
    {
      // The callback lifetime is already expired, so native dismissal is
      // presentation-only cleanup rather than an operation invariant.
      runOptionalWinRt("hiding the WinUI list-authoring dialog", [this] { _dialog.Hide(); });
    }

    handleDialogClosed();
  }
} // namespace ao::winui

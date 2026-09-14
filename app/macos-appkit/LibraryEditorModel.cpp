// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryEditorModel.h"

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/query/Parser.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/list/ListAuthoring.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <functional>
#include <utility>
#include <vector>

namespace ao::appkit
{
  LibraryEditorModel::LibraryEditorModel(rt::AppRuntime& runtime,
                                         i18n::MessageCatalog catalog,
                                         std::function<void()> onChange)
    : _runtime{runtime}, _catalog{std::move(catalog)}, _onChange{std::move(onChange)}, _form{_catalog}
  {
  }

  LibraryEditorModel::~LibraryEditorModel() = default;

  LibraryEditorState const& LibraryEditorModel::state() const noexcept
  {
    return _state;
  }

  i18n::MessageCatalog const& LibraryEditorModel::catalog() const noexcept
  {
    return _catalog;
  }

  Result<> LibraryEditorModel::beginProperties(std::vector<TrackId> ids)
  {
    if (auto res = canBegin(); !res)
    {
      return res;
    }

    auto sessionRes = uimodel::TrackAuthoringSession::begin(_runtime.library(), ids);

    if (!sessionRes)
    {
      return std::unexpected{sessionRes.error()};
    }

    _state = {
      .kind = LibraryEditorKind::Properties,
      .title = i18n::requiredFormat(_catalog, i18n::MessageId::AppKitPropertiesSelection, {{"count", ids.size()}}),
      .trackIds = std::move(ids)};
    _optTrackSession.emplace(std::move(*sessionRes));
    _form.clear();
    auto spec = uimodel::buildTrackPropertiesFormSpec(_catalog);
    spec.metadataRows.append_range(spec.propertyRows);
    auto snapshot = _runtime.library().snapshot();

    for (auto const& row : spec.metadataRows)
    {
      _form.addField(row.field, row.editorKind != uimodel::TrackPropertiesFormEditorKind::ReadonlyText);
      bool first = true;

      for (auto id : _state.trackIds)
      {
        if (auto value = snapshot.trackField(id, row.field); first)
        {
          _form.loadFirstTrackField(row.field, std::move(value));
          first = false;
        }
        else
        {
          std::ignore = _form.tryMergeTrackField(row.field, value);
        }
      }

      auto value = _form.rowView(row.field);
      _state.fields.push_back({.spec = row, .text = value.mixed ? "" : value.text, .mixed = value.mixed});
    }

    _originalTags = snapshot.selectionTags(_state.trackIds);
    _state.tags = _originalTags;
    _invalidatedSub = _optTrackSession->onInvalidated(
      [this]
      {
        _state.stale = true;
        _state.optInvalidField.reset();
        _state.error = std::string{i18n::requiredText(_catalog, i18n::MessageId::AppKitPropertiesStale)};
        publishChange();
      });
    publishChange();
    return {};
  }

  Result<> LibraryEditorModel::beginList(ListId listId, ListId parentId)
  {
    if (auto res = canBegin(); !res)
    {
      return res;
    }

    auto draft = rt::ListDraft{.parentId = parentId};

    if (listId != kInvalidListId)
    {
      auto const optList = _runtime.library().snapshot().listNode(listId);

      if (!optList)
      {
        return std::unexpected{
          Error{.code = Error::Code::InvalidInput, .message = "This saved list is no longer available."}};
      }

      draft = {.parentId = optList->parentId,
               .listId = listId,
               .name = optList->name,
               .description = optList->description,
               .expression = optList->expression};
    }

    _state = {.kind = LibraryEditorKind::List,
              .title = listId == kInvalidListId
                         ? std::string{i18n::requiredText(_catalog, i18n::MessageId::AppKitNewList)}
                         : std::string{i18n::requiredText(_catalog, i18n::MessageId::AppKitEditList)},
              .list = std::move(draft)};
    publishChange();
    return {};
  }

  Result<> LibraryEditorModel::beginDeletion(ListId listId)
  {
    if (auto res = canBegin(); !res)
    {
      return res;
    }

    _state = {.kind = LibraryEditorKind::DeleteList,
              .title = std::string{i18n::requiredText(_catalog, i18n::MessageId::AppKitDeleteList)},
              .busy = true};
    publishChange();
    _runtime.async().spawnLogged(deleteListAsync(this, listId, true), "AppKit list deletion preview");
    return {};
  }

  Result<> LibraryEditorModel::beginMembership(std::vector<TrackId> ids, ListId listId, bool remove)
  {
    if (auto res = canBegin(); !res)
    {
      return res;
    }

    auto sessionRes = uimodel::ListMembershipAuthoringSession::begin(_runtime.library(), ids);

    if (!sessionRes)
    {
      return std::unexpected{sessionRes.error()};
    }

    _state = {.kind = LibraryEditorKind::Membership,
              .title = remove ? std::string{i18n::requiredText(_catalog, i18n::MessageId::AppKitRemoveFromList)}
                              : std::string{i18n::requiredText(_catalog, i18n::MessageId::AppKitAddToList)},
              .trackIds = std::move(ids),
              .busy = true};
    publishChange();
    _runtime.async().spawnLogged(
      membershipAsync(this, std::move(*sessionRes), listId, remove), "AppKit list membership edit");
    return {};
  }

  void LibraryEditorModel::editField(std::size_t index, std::string text)
  {
    if (_state.busy || _state.stale || _closing)
    {
      return;
    }

    auto& field = _state.fields.at(index);

    if (field.spec.editorKind == uimodel::TrackPropertiesFormEditorKind::ReadonlyText)
    {
      return;
    }

    if (_state.optInvalidField == field.spec.field)
    {
      _state.optInvalidField.reset();
      _state.error.clear();
    }

    field.text = std::move(text);
    field.changed = true;
    _state.dirty = true;
    publishChange();
  }

  void LibraryEditorModel::editTags(std::vector<std::string> tags)
  {
    if (!_state.busy && !_state.stale && !_closing)
    {
      _state.tags = std::move(tags);
      _state.dirty = true;
      publishChange();
    }
  }

  void LibraryEditorModel::editList(std::string name, std::string description, std::string expression)
  {
    if (!_state.busy && !_state.stale && !_closing)
    {
      if (_state.list.expression != expression)
      {
        _state.error.clear();
        _state.listExpressionError = false;
      }

      _state.list.name = std::move(name);
      _state.list.description = std::move(description);
      _state.list.expression = std::move(expression);
      _state.dirty = true;
      publishChange();
    }
  }

  void LibraryEditorModel::save()
  {
    if (_state.busy || _state.stale || _closing || _state.completed)
    {
      return;
    }

    _state.error.clear();
    _state.optInvalidField.reset();
    _state.listExpressionError = false;

    if (_state.kind == LibraryEditorKind::List)
    {
      _state.busy = true;
      publishChange();
      _runtime.async().spawnLogged(saveListAsync(this, _state.list), "AppKit list save");
      return;
    }

    if (_state.kind == LibraryEditorKind::DeleteList && _state.optDeletion)
    {
      _state.busy = true;
      publishChange();
      _runtime.async().spawnLogged(
        deleteListAsync(this, _state.optDeletion->rootListId, false), "AppKit list deletion");
      return;
    }

    if (_state.kind != LibraryEditorKind::Properties || !_optTrackSession)
    {
      return;
    }

    for (auto const& field : _state.fields)
    {
      if (!field.changed)
      {
        continue;
      }

      auto valueRes = field.spec.editorKind == uimodel::TrackPropertiesFormEditorKind::Number
                        ? uimodel::parseUint16EditValue(field.text)
                        : uimodel::parseTextEditValue(field.text);

      if (!valueRes)
      {
        _state.optInvalidField = field.spec.field;
        _state.error = valueRes.error().message;
        publishChange();
        return;
      }

      _form.setExplicitFieldEdit(field.spec.field, std::move(*valueRes));
    }

    auto patch = rt::TrackPropertiesPatch{.metadata = _form.buildPatch()};

    for (auto const& tag : _state.tags)
    {
      if (!std::ranges::contains(_originalTags, tag))
      {
        patch.tagsToAdd.push_back(tag);
      }
    }

    for (auto const& tag : _originalTags)
    {
      if (!std::ranges::contains(_state.tags, tag))
      {
        patch.tagsToRemove.push_back(tag);
      }
    }

    _state.busy = true;
    publishChange();
    _runtime.async().spawnLogged(submitPropertiesAsync(this, _optTrackSession->submitPropertiesAsync(std::move(patch))),
                                 "AppKit track properties save");
  }

  void LibraryEditorModel::cancel()
  {
    AO_INVARIANT(!_state.busy);
    _invalidatedSub.reset();
    _optTrackSession.reset();
    _state = {};
    _form.clear();
    publishChange();
  }

  void LibraryEditorModel::shutdown()
  {
    _closing = true;
    _invalidatedSub.reset();
    _optTrackSession.reset();
  }

  Result<> LibraryEditorModel::canBegin() const
  {
    if (_closing || _state.kind != LibraryEditorKind::None)
    {
      return std::unexpected{Error{.code = Error::Code::InvalidInput, .message = "Finish the current edit first."}};
    }

    return {};
  }

  void LibraryEditorModel::publishChange() const
  {
    _onChange();
  }

  void LibraryEditorModel::finishStatus(rt::AuthoringStatus status)
  {
    _state.busy = false;

    switch (status)
    {
      case rt::AuthoringStatus::Applied:
      case rt::AuthoringStatus::NoOp:
        _state.completed = true;
        _state.dirty = false;
        _state.error.clear();
        break;
      case rt::AuthoringStatus::Busy:
        if (!_state.stale)
        {
          _state.error = std::string{i18n::requiredText(_catalog, i18n::MessageId::AppKitEditorBusy)};
        }

        break;
      case rt::AuthoringStatus::Stale:
      case rt::AuthoringStatus::Unavailable:
        _state.stale = true;
        _state.optInvalidField.reset();
        _state.error = std::string{i18n::requiredText(_catalog, i18n::MessageId::AppKitEditorStale)};
        break;
    }

    publishChange();
  }

  async::Task<void> LibraryEditorModel::submitPropertiesAsync(
    LibraryEditorModel* owner,
    async::Task<Result<uimodel::TrackPropertiesSubmitResult>> submission)
  {
    // TrackAuthoringSession and its synchronous invalidation observer share the callback executor.
    co_await owner->_runtime.async().resumeOnCallbackExecutorAsync();
    auto res = co_await std::move(submission);
    co_await owner->_runtime.async().resumeOnCallbackExecutorAsync();

    if (!owner->_closing)
    {
      if (res)
      {
        owner->finishStatus(res->status);
      }
      else
      {
        owner->_state.busy = false;

        if (!owner->_state.stale)
        {
          owner->_state.error = res.error().message;
        }

        owner->publishChange();
      }
    }
  }

  async::Task<void> LibraryEditorModel::saveListAsync(LibraryEditorModel* owner, rt::ListDraft draft)
  {
    auto res = co_await uimodel::saveListAsync(&owner->_runtime.library(), std::move(draft));
    co_await owner->_runtime.async().resumeOnCallbackExecutorAsync();

    if (!owner->_closing)
    {
      owner->_state.busy = false;

      if (res)
      {
        owner->_state.savedListId = *res;
        owner->finishStatus(rt::AuthoringStatus::Applied);
      }
      else
      {
        if (!owner->_state.stale)
        {
          owner->_state.error = res.error().message;
          auto const& expression = owner->_state.list.expression;
          owner->_state.listExpressionError = !query::parse(expression.empty() ? "true" : expression);
        }

        owner->publishChange();
      }
    }
  }

  async::Task<void> LibraryEditorModel::deleteListAsync(LibraryEditorModel* owner, ListId listId, bool preview)
  {
    auto res = co_await (preview ? uimodel::previewListDeletionAsync(&owner->_runtime.library(), listId, true)
                                 : uimodel::deleteListAsync(&owner->_runtime.library(), listId, true));
    co_await owner->_runtime.async().resumeOnCallbackExecutorAsync();

    if (!owner->_closing)
    {
      owner->_state.busy = false;

      if (!res)
      {
        if (!owner->_state.stale)
        {
          owner->_state.error = res.error().message;
        }

        owner->publishChange();
      }
      else if (preview)
      {
        owner->_state.optDeletion = std::move(*res);
        owner->publishChange();
      }
      else
      {
        owner->finishStatus(rt::AuthoringStatus::Applied);
      }
    }
  }

  async::Task<void> LibraryEditorModel::membershipAsync(LibraryEditorModel* owner,
                                                        uimodel::ListMembershipAuthoringSession session,
                                                        ListId listId,
                                                        bool remove)
  {
    auto res = co_await (remove ? session.removeFromListAsync(listId) : session.addToListAsync(listId));
    co_await owner->_runtime.async().resumeOnCallbackExecutorAsync();

    if (!owner->_closing)
    {
      if (res)
      {
        owner->finishStatus(res->status);
      }
      else
      {
        owner->_state.busy = false;

        if (!owner->_state.stale)
        {
          owner->_state.error = res.error().message;
        }

        owner->publishChange();
      }
    }
  }
} // namespace ao::appkit

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryEditorModel.h"

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/list/ListAuthoring.h>
#include <ao/uimodel/library/property/TagEdit.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/property/TrackPropertiesPreparation.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
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

  std::size_t LibraryEditorModel::propertyTargetCount() const noexcept
  {
    return _state.kind == LibraryEditorKind::Properties ? _form.targetCount() : 0;
  }

  std::optional<TrackId> LibraryEditorModel::singlePropertyTarget() const noexcept
  {
    if (propertyTargetCount() == 1 && _optTrackSession)
    {
      return _optTrackSession->targetIds().front();
    }

    return std::nullopt;
  }

  Result<> LibraryEditorModel::beginProperties(std::span<TrackId const> const ids)
  {
    if (auto res = canBegin(); !res)
    {
      return res;
    }

    auto optSession = std::optional<uimodel::TrackAuthoringSession>{};
    auto const spec = uimodel::buildTrackPropertiesFormSpec(_catalog);
    auto baseline = uimodel::TrackPropertiesFormModel{_catalog};
    auto fields = std::vector<LibraryEditorField>{};
    auto tags = std::vector<std::string>{};

    {
      auto snapshot = _runtime.library().snapshot();

      auto sessionRes = uimodel::prepareTrackProperties(_runtime.library(), snapshot, ids, spec, baseline);

      if (!sessionRes)
      {
        return std::unexpected{sessionRes.error()};
      }

      optSession.emplace(std::move(*sessionRes));

      auto rows = spec.metadataRows;
      rows.append_range(spec.propertyRows);
      fields.reserve(rows.size());

      for (auto const& row : rows)
      {
        auto const value = baseline.rowView(row.field);
        fields.push_back({.spec = row, .text = value.mixed ? "" : value.text, .mixed = value.mixed});
      }

      tags = snapshot.selectionTags(optSession->targetIds());
    }

    auto& session = *optSession;
    auto invalidatedSub = session.onInvalidated(
      [this]
      {
        if (_state.kind != LibraryEditorKind::Properties)
        {
          return;
        }

        _state.stale = true;
        _state.optInvalidField.reset();
        _state.error = std::string{i18n::requiredText(_catalog, i18n::MessageId::AppKitPropertiesStale)};
        publishChange();
      });

    if (!session.isCurrent())
    {
      return makeError(Error::Code::InvalidState, "The library changed while Track Properties was opening");
    }

    auto state = LibraryEditorState{
      .kind = LibraryEditorKind::Properties,
      .title =
        i18n::requiredFormat(_catalog, i18n::MessageId::AppKitPropertiesSelection, {{"count", baseline.targetCount()}}),
      .fields = std::move(fields),
      .tags = tags,
    };
    _form = std::move(baseline);
    _originalTags = std::move(tags);
    _optTrackSession.emplace(std::move(session));
    _invalidatedSub = std::move(invalidatedSub);
    _state = std::move(state);
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

  Result<> LibraryEditorModel::beginMembership(std::span<TrackId const> const ids, ListId listId, bool remove)
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
              .title = std::string{i18n::requiredText(
                _catalog, remove ? i18n::MessageId::AppKitRemoveFromList : i18n::MessageId::ListAddToPlaylist)},
              .membershipTargetCount = sessionRes->targetIds().size(),
              .busy = true};
    publishChange();
    _runtime.async().spawnLogged(
      membershipAsync(this, std::move(*sessionRes), listId, remove), "AppKit list membership edit");
    return {};
  }

  void LibraryEditorModel::editField(std::size_t index, std::string text)
  {
    updateField(index, std::move(text), FieldAction::Input);
  }

  void LibraryEditorModel::clearField(std::size_t index)
  {
    updateField(index, {}, FieldAction::Clear);
  }

  bool LibraryEditorModel::canRestoreField(std::size_t index) const noexcept
  {
    return propertyTargetCount() > 1 && !_state.busy && !_state.stale && !_state.completed && !_closing &&
           !_form.creditsEditor().isEditing() && index < _state.fields.size() && _state.fields[index].changed &&
           _state.fields[index].spec.editorKind != uimodel::TrackPropertiesFormEditorKind::ReadonlyText;
  }

  void LibraryEditorModel::restoreField(std::size_t index)
  {
    if (canRestoreField(index))
    {
      updateField(index, {}, FieldAction::Restore);
    }
  }

  void LibraryEditorModel::updateField(std::size_t index, std::string text, FieldAction action)
  {
    if (_state.kind != LibraryEditorKind::Properties || _state.busy || _state.stale || _state.completed || _closing)
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
      _state.error = _tagInputError;
    }

    field.optInputError.reset();
    field.changed = true;
    field.text = std::move(text);

    if (action == FieldAction::Clear)
    {
      _form.clearFieldEdit(field.spec.field);
    }
    else if (action == FieldAction::Restore)
    {
      _form.restoreFieldEdit(field.spec.field);
      field.changed = false;
    }
    else if (auto valueRes = uimodel::parseTrackPropertiesFieldInput(field.spec.editorKind, field.text); valueRes)
    {
      _form.setFieldInput(field.spec.field, std::move(*valueRes));
      field.changed = _form.hasFieldChange(field.spec.field);
    }
    else
    {
      // Retain invalid native text for correction, never the previous valid replacement for retry.
      _form.restoreFieldEdit(field.spec.field);
      field.optInputError.emplace(uimodel::trackPropertiesInputHint(_catalog, field.spec.editorKind));
    }

    if (!field.changed)
    {
      field.text = field.mixed ? "" : _form.rowView(field.spec.field).text;
    }

    updatePropertiesDirty();
    publishChange();
  }

  void LibraryEditorModel::updatePropertiesDirty()
  {
    auto const hasInvalidInput = std::ranges::any_of(
      _state.fields, [](LibraryEditorField const& field) { return field.optInputError.has_value(); });
    _state.dirty = _tagInputPending || !_tagInputError.empty() || hasInvalidInput || _form.hasEffectiveChanges() ||
                   !uimodel::diffCommonTags(_originalTags, _state.tags).empty();
  }

  void LibraryEditorModel::markTagInputPending(std::string error)
  {
    if (_state.kind == LibraryEditorKind::Properties && !_state.busy && !_state.stale && !_state.completed && !_closing)
    {
      _tagInputPending = true;

      if (!error.empty())
      {
        setTagInputError(std::move(error));
      }

      updatePropertiesDirty();
      publishChange();
    }
  }

  void LibraryEditorModel::setTagInputError(std::string error)
  {
    // A scalar diagnostic belongs to its row. Clearing a tag error must not erase an unrelated diagnostic.
    if (!_state.optInvalidField && (!error.empty() || _state.error == _tagInputError))
    {
      _state.error = error;
    }

    _tagInputError = std::move(error);
  }

  bool LibraryEditorModel::tryEditTags(std::vector<std::string> tags)
  {
    if (_state.kind != LibraryEditorKind::Properties || _state.busy || _state.stale || _state.completed || _closing)
    {
      return false;
    }

    _tagInputPending = false;
    auto canonical = std::vector<std::string>{};
    bool accepted = true;

    for (auto const& tag : tags)
    {
      if (!uimodel::appendCommonTag(canonical, tag).has_value())
      {
        accepted = false;
        break;
      }
    }

    setTagInputError(
      accepted ? std::string{}
               : std::string{i18n::requiredText(_catalog, i18n::MessageId::TrackPropertiesTextNormalizationFailed)});
    _state.tags = accepted ? std::move(canonical) : std::move(tags);
    updatePropertiesDirty();
    publishChange();
    return accepted;
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

  uimodel::TrackCreditSections LibraryEditorModel::creditSections() const
  {
    return _form.creditSections();
  }

  std::vector<uimodel::TrackSegmentDisplayRow> const& LibraryEditorModel::segmentRows() const noexcept
  {
    return _form.segmentRows();
  }

  uimodel::TrackCreditsEditorModel& LibraryEditorModel::creditsEditor() noexcept
  {
    return _form.creditsEditor();
  }

  Result<> LibraryEditorModel::beginCreditsEdit(std::bitset<library::kCreditKindCount> kinds)
  {
    if (_state.kind != LibraryEditorKind::Properties || _state.busy || _state.stale || _closing || _state.completed)
    {
      return makeError(Error::Code::InvalidState, "Credits editing requires current, idle Track Properties");
    }

    auto res = _form.beginCreditsEdit(kinds);

    // Report a refused baseline like GTK and WinUI instead of ignoring the click.
    if (!res)
    {
      _state.error = res.error().message;
    }

    publishChange();
    return res;
  }

  Result<> LibraryEditorModel::acceptCreditsEdit()
  {
    if (_state.busy || _state.stale || _closing)
    {
      return makeError(Error::Code::InvalidState, "Credits editing requires current, idle Track Properties");
    }

    auto res = _form.acceptCreditsEdit();

    if (res)
    {
      updatePropertiesDirty();
    }

    publishChange();
    return res;
  }

  void LibraryEditorModel::cancelCreditsEdit()
  {
    _form.cancelCreditsEdit();
    publishChange();
  }

  void LibraryEditorModel::save()
  {
    // A token still in the native field editor has not entered the patch. Only tryEditTags at
    // end-edit can release this guard; leave any validation error visible in the meantime.
    if (_state.busy || _state.stale || _closing || _state.completed || _form.creditsEditor().isEditing() ||
        _tagInputPending)
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
      if (field.optInputError)
      {
        _state.optInvalidField = field.spec.field;
        _state.error = *field.optInputError;
        publishChange();
        return;
      }
    }

    if (!_tagInputError.empty())
    {
      _state.error = _tagInputError;
      publishChange();
      return;
    }

    auto changes = uimodel::diffCommonTags(_originalTags, _state.tags);
    auto patch = rt::TrackPropertiesPatch{.metadata = _form.buildPatch(),
                                          .tagsToAdd = std::move(changes.tagsToAdd),
                                          .tagsToRemove = std::move(changes.tagsToRemove)};

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
    _tagInputPending = false;
    _tagInputError.clear();
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
        else if (!res.error().message.empty())
        {
          // Invalidation already supplied the reopen instruction. Keep the write failure as its cause.
          owner->_state.error = res.error().message + '\n' + owner->_state.error;
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
          // List authoring preserves the query parser/compiler's FormatRejected code.
          owner->_state.listExpressionError = res.error().code == Error::Code::FormatRejected;
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
        // Format before the synchronous change callback; reentrant shutdown retires the post.
        auto const notification = uimodel::listMembershipEditNotification(owner->_catalog, *res);
        owner->finishStatus(res->status);

        if (owner->_closing)
        {
          co_return;
        }

        owner->_runtime.notifications().post(notification.severity, notification.text, notification.lifetime);
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

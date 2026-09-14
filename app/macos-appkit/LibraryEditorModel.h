// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ao::appkit
{
  enum class LibraryEditorKind : std::uint8_t
  {
    None,
    Properties,
    List,
    DeleteList,
    Membership,
  };

  struct LibraryEditorField final
  {
    uimodel::TrackPropertiesFormRow spec;
    std::string text;
    bool mixed = false;
    bool changed = false;
  };

  struct LibraryEditorState final
  {
    LibraryEditorKind kind = LibraryEditorKind::None;
    std::string title{};
    std::string error{};
    std::optional<rt::TrackField> optInvalidField{};
    std::vector<LibraryEditorField> fields{};
    std::vector<std::string> tags{};
    std::vector<TrackId> trackIds{};
    rt::ListDraft list{};
    std::optional<rt::DeleteListSubtreeReply> optDeletion{};
    ListId savedListId = kInvalidListId;
    bool listExpressionError = false;
    bool dirty = false;
    bool busy = false;
    bool stale = false;
    bool completed = false;
  };

  // One native editor at a time. The owning LibrarySession retains this object
  // through runtime shutdown and the final callback drain; observers never enter AppKit.
  // TrackAuthoringSession submission and invalidation state are callback-executor confined.
  class [[nodiscard]] LibraryEditorModel final
  {
  public:
    LibraryEditorModel(rt::AppRuntime& runtime, i18n::MessageCatalog catalog, std::function<void()> onChange);
    ~LibraryEditorModel();
    LibraryEditorModel(LibraryEditorModel const&) = delete;
    LibraryEditorModel& operator=(LibraryEditorModel const&) = delete;
    LibraryEditorModel(LibraryEditorModel&&) = delete;
    LibraryEditorModel& operator=(LibraryEditorModel&&) = delete;

    LibraryEditorState const& state() const noexcept;
    i18n::MessageCatalog const& catalog() const noexcept;
    Result<> beginProperties(std::vector<TrackId> ids);
    Result<> beginList(ListId listId = kInvalidListId, ListId parentId = kInvalidListId);
    Result<> beginDeletion(ListId listId);
    Result<> beginMembership(std::vector<TrackId> ids, ListId listId, bool remove);
    void editField(std::size_t index, std::string text);
    void editTags(std::vector<std::string> tags);
    void editList(std::string name, std::string description, std::string expression);
    void save();
    void cancel();
    void shutdown();

  private:
    Result<> canBegin() const;
    void publishChange() const;
    void finishStatus(rt::AuthoringStatus status);
    static async::Task<void> submitPropertiesAsync(
      LibraryEditorModel* owner,
      async::Task<Result<uimodel::TrackPropertiesSubmitResult>> submission);
    static async::Task<void> saveListAsync(LibraryEditorModel* owner, rt::ListDraft draft);
    static async::Task<void> deleteListAsync(LibraryEditorModel* owner, ListId listId, bool preview);
    static async::Task<void> membershipAsync(LibraryEditorModel* owner,
                                             uimodel::ListMembershipAuthoringSession session,
                                             ListId listId,
                                             bool remove);

    rt::AppRuntime& _runtime;
    i18n::MessageCatalog _catalog;
    std::function<void()> _onChange;
    uimodel::TrackPropertiesFormModel _form;
    LibraryEditorState _state;
    std::vector<std::string> _originalTags;
    std::optional<uimodel::TrackAuthoringSession> _optTrackSession;
    async::Subscription _invalidatedSub;
    bool _closing = false;
  };
} // namespace ao::appkit
